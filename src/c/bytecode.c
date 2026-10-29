/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) --
 * see ~/.claude/plans/soft-wishing-engelbart.md. Nearly verbatim: this
 * file is pure portable (de)serialization of an IrProgram's op-id/
 * symbol-by-name form (never a raw handler address or process-local
 * symbol id -- see bytecode.h's own header comment), so the only
 * change needed for `ll` is handler_to_op_id's reverse lookup table
 * below -- ll's own ir_link() (src/llvm/dispatch_table.c) resolves op-
 * ids against `ll_handler_table`, not asnix's `ll_handler_table`
 * (both tables are populated identically -- OP_COUNT-sized, indexed by
 * op-id -- this is a naming difference only, not a layout one). */
#include "bytecode.h"

#include <stdlib.h>
#include <string.h>

#include "ll/arena.h"
#include "ll/heap.h"
#include "builtins_shared.h"
#include "compiler.h" /* marker-reparse addition: ll_lazy_thunk_descriptor_*/
                      /* ll_lazy_scope_push_frame/ll_make_cached_lazy_thunk_descriptor --
                       * see PTAG_OP_LAZY_MARKER's own comment below */
#include "op_dispatch.h" /* ll_handler_table -- see this file's own header comment */
#include "ops/ops.h"      /* ll_op_push_int_run -- see handler_to_op_id's own comment */
#include "symtab.h"
#include "value_util.h"

/* ---------------------------------------------------------------------
 * Growable write buffer / bounded read cursor.
 * --------------------------------------------------------------------- */
typedef struct {
    uint8_t *buf;
    size_t len, cap;
    int failed;
} WBuf;

static void wbuf_reserve(WBuf *b, size_t extra) {
    if (b->failed) return;
    if (b->len + extra <= b->cap) return;
    size_t newcap = b->cap ? b->cap * 2 : 4096;
    while (newcap < b->len + extra) newcap *= 2;
    uint8_t *nb = realloc(b->buf, newcap);
    if (!nb) {
        b->failed = 1;
        return;
    }
    b->buf = nb;
    b->cap = newcap;
}

static void wbuf_bytes(WBuf *b, const void *p, size_t n) {
    wbuf_reserve(b, n);
    if (b->failed) return;
    memcpy(b->buf + b->len, p, n);
    b->len += n;
}

static void wbuf_u8(WBuf *b, uint8_t v) {
    wbuf_bytes(b, &v, 1);
}

/* Variable-width (LEB128-style) integer encoding for every opcode/
 * operand/length/symbol-id field, rather than a fixed 8-byte int64:
 * most fields here (opcodes, small operand values, lengths, symbol ids)
 * fit in 1-2 bytes. Since `IrOp.a`/`.b` and `VTAG_INT`'s payload are
 * genuinely signed (jump deltas can be negative; a `PUSH_INT` operand is
 * an arbitrary signed 64-bit Nix integer), this adds a zigzag step (the
 * standard protobuf `sint64` trick: maps 0,-1,1,-2,2,... to 0,1,2,3,4,...)
 * before the same LEB128 varint, folding "signed small magnitude" into
 * "small unsigned", while still correctly round-tripping the full
 * int64_t range (including INT64_MIN/INT64_MAX) at a bounded worst case
 * of 10 bytes.
 *
 * Every call site below still calls plain wbuf_i64/rcur_i64 with an
 * int64_t -- only these two functions' own wire format changed, so this
 * is the entire diff against a fixed-width encoding. */
static uint64_t zigzag_encode(int64_t v) {
    return ((uint64_t)v << 1) ^ (uint64_t)(v >> 63);
}

static int64_t zigzag_decode(uint64_t v) {
    return (int64_t)((v >> 1) ^ (~(v & 1) + 1));
}

static void wbuf_varu64(WBuf *b, uint64_t v) {
    while (v >= 0x80) {
        wbuf_u8(b, (uint8_t)(v & 0x7f) | 0x80);
        v >>= 7;
    }
    wbuf_u8(b, (uint8_t)v);
}

static void wbuf_i64(WBuf *b, int64_t v) {
    wbuf_varu64(b, zigzag_encode(v));
}

static void wbuf_bytes_with_len(WBuf *b, const char *p, size_t n) {
    wbuf_i64(b, (int64_t)n);
    wbuf_bytes(b, p, n);
}

typedef struct {
    const uint8_t *buf;
    size_t len, pos;
    int failed;
} RCur;

static void rcur_bytes(RCur *r, void *out, size_t n) {
    if (r->failed || n > r->len - r->pos) {
        r->failed = 1;
        memset(out, 0, n);
        return;
    }
    memcpy(out, r->buf + r->pos, n);
    r->pos += n;
}

static uint8_t rcur_u8(RCur *r) {
    uint8_t v = 0;
    rcur_bytes(r, &v, 1);
    return v;
}

/* Bounded to 10 bytes (ceil(64/7)) -- a malformed/corrupt entry whose
 * continuation bit never clears within that many bytes is rejected
 * (r->failed = 1) rather than looping or reading out of bounds; the
 * caller's existing "any anomaly -> NULL, never a crash" contract
 * (ll_bytecode_deserialize) covers the rest. */
static uint64_t rcur_varu64(RCur *r) {
    uint64_t v = 0;
    int shift = 0;
    for (int i = 0; i < 10; i++) {
        uint8_t byte = rcur_u8(r);
        if (r->failed) return 0;
        if (shift < 64) v |= ((uint64_t)(byte & 0x7f)) << shift;
        if ((byte & 0x80) == 0) return v;
        shift += 7;
    }
    r->failed = 1;
    return 0;
}

static int64_t rcur_i64(RCur *r) {
    return zigzag_decode(rcur_varu64(r));
}

/* Arena-owned (never freed -- same lifetime convention every other
 * compiled-in string constant already uses) NUL-free byte copy of the
 * next length-prefixed field; `*out_len` is 0 and the cursor is marked
 * failed on any bounds violation. */
static const char *rcur_bytes_with_len(RCur *r, size_t *out_len) {
    int64_t n = rcur_i64(r);
    if (r->failed || n < 0 || (size_t)n > r->len - r->pos) {
        r->failed = 1;
        *out_len = 0;
        return NULL;
    }
    char *mem = ll_arena_alloc((size_t)n);
    rcur_bytes(r, mem, (size_t)n);
    *out_len = (size_t)n;
    return mem;
}

/* ---------------------------------------------------------------------
 * Value (de)serialization -- tag + content, never raw pointer bits.
 * Fails (-1) on any heap-pointer kind that should never appear in a
 * compile-time constant pool (List/Closure/Thunk/PrimOp): compile_expr's
 * every ib_push_const call site is Float, a literal String, a literal
 * Path, or (AST_CURPOS only) a small Attrs built from those -- confirmed
 * by reading every push_const call site in compiler.c. This format
 * refuses to guess at anything else rather than half-support it.
 * --------------------------------------------------------------------- */
enum { VTAG_INT = 0, VTAG_IMM = 1, VTAG_STRING = 2, VTAG_PATH = 3, VTAG_FLOAT = 4, VTAG_ATTRS = 5 };

static int write_value(WBuf *b, Value v) {
    int tag = ll_tag_of(v);
    if (tag == LL_TAG_INT) {
        wbuf_u8(b, VTAG_INT);
        wbuf_i64(b, ll_int_value(v));
        return 0;
    }
    if (tag == LL_TAG_IMM) {
        wbuf_u8(b, VTAG_IMM);
        wbuf_u8(b, (uint8_t)ll_imm_subtag(v));
        return 0;
    }
    if (tag == LL_TAG_PTR) {
        if (ll_value_is_float(v)) {
            double d = ll_float_value(v);
            wbuf_u8(b, VTAG_FLOAT);
            wbuf_bytes(b, &d, 8);
            return 0;
        }
        int64_t len;
        const char *bytes = ll_ptr_bytes(v, &len);
        wbuf_u8(b, ll_value_is_path(v) ? VTAG_PATH : VTAG_STRING);
        wbuf_bytes_with_len(b, bytes, (size_t)len);
        return 0;
    }
    if (tag == LL_TAG_ATTRS) {
        wbuf_u8(b, VTAG_ATTRS);
        int64_t n = ll_bshared_attrs_count(v);
        wbuf_i64(b, n);
        for (int64_t i = 0; i < n; i++) {
            uint32_t sym = ll_bshared_attrs_sym(v, i);
            size_t name_len;
            const char *name = ll_sym_name(sym, &name_len);
            wbuf_bytes_with_len(b, name, name_len);
            if (write_value(b, ll_bshared_attrs_value(v, i)) != 0) return -1;
        }
        return 0;
    }
    return -1; /* LIST/CLOSURE/THUNK/PRIMOP: never a real pool constant today */
}

/* ll_bshared_make_attrs unconditionally insertion-sorts its syms/vals
 * arrays in place before building the heap object (builtins_shared.c) --
 * so re-interned, no-longer-sorted-by-new-id entries come out correctly
 * sorted with no extra step needed here. This is the ONE place in this
 * file that's allowed to rely on that -- every other attrset-shaped
 * reconstruction below (OP_MK_ATTRS) does NOT go through
 * ll_bshared_make_attrs directly (it must also interleave freshly
 * re-ordered value-producing ops into the surrounding bytecode stream,
 * not just build one heap object) and sorts explicitly itself. */
static int read_value(RCur *r, Value *out) {
    uint8_t tag = rcur_u8(r);
    if (r->failed) return -1;
    switch (tag) {
        case VTAG_INT:
            *out = ll_make_int(rcur_i64(r));
            return r->failed ? -1 : 0;
        case VTAG_IMM: {
            uint8_t sub = rcur_u8(r);
            if (r->failed || sub > LL_IMM_BLACKHOLE) return -1;
            *out = ll_make_imm(sub);
            return 0;
        }
        case VTAG_FLOAT: {
            double d;
            rcur_bytes(r, &d, 8);
            if (r->failed) return -1;
            *out = ll_make_float_value(d);
            return 0;
        }
        case VTAG_STRING:
        case VTAG_PATH: {
            size_t len;
            const char *bytes = rcur_bytes_with_len(r, &len);
            if (r->failed) return -1;
            *out = (tag == VTAG_PATH) ? ll_make_path_value(bytes, len) : ll_make_string_value(bytes, len);
            return 0;
        }
        case VTAG_ATTRS: {
            int64_t n = rcur_i64(r);
            if (r->failed || n < 0 || n > 1000000) return -1;
            uint32_t *syms = n > 0 ? malloc((size_t)n * sizeof(uint32_t)) : NULL;
            Value *vals = n > 0 ? malloc((size_t)n * sizeof(Value)) : NULL;
            int rc = 0;
            for (int64_t i = 0; i < n; i++) {
                size_t name_len;
                const char *name = rcur_bytes_with_len(r, &name_len);
                if (r->failed) {
                    rc = -1;
                    break;
                }
                syms[i] = ll_intern(name, name_len);
                if (read_value(r, &vals[i]) != 0) {
                    rc = -1;
                    break;
                }
            }
            if (rc == 0) *out = ll_bshared_make_attrs(syms, vals, n);
            free(syms);
            free(vals);
            return rc;
        }
        default:
            return -1;
    }
}

/* ---------------------------------------------------------------------
 * Op-id <-> handler-pointer translation (ir_link's own mapping, reversed
 * here for serialization; re-applied for deserialization via ir_link
 * itself).
 * --------------------------------------------------------------------- */
static int handler_to_op_id(void *handler) {
    /* ir_link()'s own peephole fusion (dispatch_table.c) rewrites the
     * FIRST op of a qualifying run of >= 4 consecutive OP_PUSH_INTs to
     * `ll_op_push_int_run` directly (never through `ll_handler_table` --
     * see that function's own comment), so the loop below alone would
     * return -1 for it, making program_op_count/write_one_op refuse to
     * cache ANY program containing such a run. Reporting it as plain
     * OP_PUSH_INT here is deliberate, not a shortcut: write_one_op's own
     * OP_PUSH_INT case only ever reads THIS op's own `pool[op->b]`
     * (the run's first literal) -- the run's OTHER (run_len - 1)
     * literals live in the array's own NEXT ops (ops[i+1..], each
     * already individually linked to the ordinary `ll_handler_table
     * [OP_PUSH_INT]` by ir_link itself, per that function's own
     * comment), which this same write_program walk visits right
     * afterwards as perfectly ordinary, individually-valid OP_PUSH_INT
     * entries -- so the serialized form is just the UNFUSED run,
     * written as `run_len` consecutive plain PTAG_OP_PUSHINT entries.
     * On load, read_one_tagged_op's own PTAG_OP_PUSHINT case
     * db_pool_add()s each literal back-to-back with no other pool
     * traffic interleaved, so their freshly-assigned pool indices come
     * out consecutive again exactly like the original compile did --
     * ir_link() (called once on the whole deserialized program right
     * after this read, ll_bytecode_deserialize's own last step)
     * re-discovers and re-fuses the identical run on its own, with no
     * separate handling needed here at all. */
    if (handler == (void *)ll_op_push_int_run) return OP_PUSH_INT;
    for (int i = 0; i < OP_COUNT; i++) {
        if (ll_handler_table[i] == handler) return i;
    }
    return -1;
}

/* ---------------------------------------------------------------------
 * Program (de)serialization.
 *
 * Pool-referencing ops (every one enumerated in ir.h's own OP_* comments
 * -- OP_PUSH_CONST, OP_MK_ATTRS, OP_MK_THUNK, OP_LAMBDA, OP_ASSERT_FAIL,
 * OP_CHECK_FORMALS, OP_REC_OVERRIDES_MERGE) carry their pool payload
 * INLINE, immediately after the op itself, instead of in a separate pool
 * block: pool indices are assigned by strict append order
 * (irbuild.c's ib_pool_add), so replaying pool-payload emission in the
 * same op order during deserialization reconstructs an equivalent
 * relative layout with no index bookkeeping needed at all.
 *
 * OP_MK_ATTRS is the one case needing more than "copy the referenced
 * pool slots verbatim, substituting names for ids": asnix's own
 * OP_MK_ATTRS (src/asm/ops/op_mk_attrs.S) assumes its pool-stored symbol
 * list AND the immediately-preceding `count` value-pushing ops are
 * BOTH already in the same sorted-by-symbol-id order -- it performs no
 * sort of its own, just pops VALSTACK in reverse and pairs position-for-
 * position with the pool's symbol list (see that file's own comment).
 * Symbol ids are assigned per-process by first-seen order (symtab.c),
 * so re-interning a cached entry's names in a FRESH process does not
 * generally preserve "sorted by id" -- the ids themselves differ, and
 * there is no general way to resort a flat bytecode stream's symbol list
 * without also reordering the exact ops that produced each matching
 * value. Fixed by writing an attrset literal's `count` (name, value-op)
 * pairs as an explicitly grouped, UNSORTED unit (write_program's
 * OP_MK_ATTRS case) and sorting by the freshly interned ids only once
 * loaded (read_program's OP_MK_ATTRS case) -- the one point where "the
 * new ids" actually exist. Verified this grouping is always exactly
 * `count` single ops, directly adjacent, with nothing else interleaved:
 * compiler.c's AST_ATTRSET binding loop emits exactly one op per binding
 * (either a thunk-free literal's own single push op -- M90's
 * is_thunk_free_literal -- or exactly one ib_mk_thunk call wrapping a
 * self-contained child program), never a multi-op inline sequence.
 *
 * OP_CHECK_FORMALS's two symbol lists are independently sortable (no
 * coupling to any other op's execution order -- op_check_formals.S pops
 * an Attrs and pushes nothing back, it doesn't correlate with a
 * preceding value-push sequence the way OP_MK_ATTRS does) and
 * OP_REC_OVERRIDES_MERGE's symbol list is positional, not sorted (its
 * own ir.h comment: "in ORIGINAL (OP_FRAME_SET slot) order, NOT
 * sorted" -- tied to OP_FRAME_SET slot indices assigned earlier in the
 * same block), so both are handled with a simpler name-for-name
 * substitution that preserves list order exactly, no resort needed.
 * --------------------------------------------------------------------- */

enum {
    PTAG_OP = 0,        /* ordinary op, no pool payload */
    PTAG_OP_CONST = 1,  /* OP_PUSH_CONST: one Value follows */
    PTAG_OP_THUNK = 2,  /* OP_MK_THUNK: one child program follows */
    PTAG_OP_LAMBDA = 3, /* OP_LAMBDA: arity (a) already written as the op's `a`; one child
                          * program + one Value (formals) follow */
    PTAG_OP_ASSERT = 4, /* OP_ASSERT_FAIL: one length-prefixed message follows */
    PTAG_OP_CHECKF = 5, /* OP_CHECK_FORMALS: total_count + two independently-sortable name lists */
    PTAG_OP_RECOVR = 6, /* OP_REC_OVERRIDES_MERGE: n positional names + one plain int */
    PTAG_OP_ATTRS = 7,  /* OP_MK_ATTRS: count UNSORTED (name, value-op) pairs, grouped */
    PTAG_OP_SYM_A = 8,  /* a plain-int `a` operand carries a bare symbol id (not a pool
                          * index): OP_SELECT, OP_HAS_ATTR -- `a` is written/read as a
                          * length-prefixed name instead of a raw int; `b` passes through
                          * unchanged. Found the hard way: without this, a cached
                          * OP_SELECT's stale symbol id replays verbatim in a fresh
                          * process and can collide with whatever THAT process's symtab
                          * happens to assign the same small integer to -- confirmed via
                          * `builtins.attrNames { y=1; x=2; foo=3; }` failing with
                          * "attribute 'y' missing" on a cache hit, 'y' being whatever
                          * this process coincidentally interned at the stale id
                          * `builtins.attrNames` itself was compiled against. */
    PTAG_OP_LAZY_MARKER = 12, /* Lazy-thunk + bytecode-cache reconciliation: an OP_MK_THUNK
                          * whose child is an UNRESOLVED OP_LAZY_THUNK_BODY stub (compiler.c's
                          * emit_attrset_field_value), written as a portable MARKER instead of
                          * refusing to cache the whole program (program_op_count's own prior,
                          * unconditional refusal -- still the fallback here whenever this
                          * marker can't be built, see write_lazy_thunk_marker). Payload: the
                          * reparse byte offset (one varint), `file_path`/`base_dir` (two
                          * length-prefixed byte strings -- needed so a later force's
                          * ll_parse_subexpr/compile_expr gets the exact same `import`/
                          * `__curPos` context the live path's own descriptor would have
                          * captured), then the ScopeFrame chain the deferred value's lexical
                          * lookups need, OUTERMOST frame first: frame-count (one varint), then
                          * per frame: count + is_with + has_overrides (three varints) followed
                          * by `count` length-prefixed name byte strings. On load, this becomes
                          * a LazyThunkDescriptor with `body == NULL` -- ll_compile_lazy_
                          * thunk_body's own first-force check reparses `file_path`'s source
                          * text (reopened and memoized once per distinct path, see compiler.c's
                          * reparse_source_for_file -- NOT the bytecode-cache entry's own mmap,
                          * which is already unmapped by the time deserialization returns) at
                          * that byte offset before compiling, instead of using an already-
                          * parsed Ast it never had. */
    PTAG_OP_SYM_B = 9,  /* same, but the bare symbol id is in `b` -- OP_WITH_TRY,
                          * OP_SELECT_OR, OP_HAS_ATTR_IF (all three also have a real
                          * jump-delta in `a`, passed through unchanged). */
    PTAG_OP_PUSHINT = 10, /* OP_PUSH_INT -- one raw (untagged) int64 literal follows,
                          * written/read exactly like PTAG_OP_CONST except the payload is a
                          * bare int64 (write_value/read_value's Value.raw encoding), not a
                          * fully-formed, already-tagged Value -- see irbuild.c's ib_push_int. */
    PTAG_OP_FORMALSMAP = 11, /* `ll`-port addition (OP_MK_FORMALSMAP has no asnix equivalent --
                          * see ir.h's own comment): nf (a) + nf pool entries, each either a
                          * length-prefixed symbol NAME (re-interned on load, same reasoning
                          * as PTAG_OP_SYM_A/_B) or the sentinel length -1 for a formal that
                          * doesn't use this mechanism (irbuild.c's ib_mk_formalsmap: -1 means
                          * "has a default, left to the per-formal thunk path"). POSITIONAL,
                          * like OP_REC_OVERRIDES_MERGE's own list -- one entry per formal
                          * SLOT in original order, never re-sorted (unlike OP_CHECK_FORMALS's
                          * two lists, which op_check_formals.S binary-searches and so must
                          * stay sorted; this op instead writes directly into env->values[i]
                          * by POSITION, see op_misc.c's ll_op_mk_formalsmap). Found the same
                          * way every other symbol-id-portability bug in this file was found:
                          * `bytecode.c` was ported from asnix nearly verbatim, and asnix has
                          * no OP_MK_FORMALSMAP at all to have ever needed a case for -- it
                          * silently fell through to the generic PTAG_OP default (which writes
                          * `op->b`'s raw, PROCESS-LOCAL pool index as if it were a portable
                          * integer, and never serializes what's actually AT that pool index
                          * at all), desyncing every op downstream of it. Confirmed via a
                          * minimal repro: `{ pkgs }: [ pkgs.a ]` cached then replayed in a
                          * fresh process aborted with "'.' selection target is not an
                          * attrset (got an integer)" -- any attrset-pattern lambda whose body
                          * does anything beyond the single formal access this format's own
                          * "OP_CHECK_FORMALS only" test case happened to cover. */
};

static int write_program(WBuf *b, IrProgram *prog);
static int write_one_op(WBuf *b, IrOp *op, int64_t *pool);

/* Writes PTAG_OP_LAZY_MARKER for `desc` (an unresolved OP_LAZY_THUNK_BODY
 * stub's descriptor, opaque here -- see compiler.h's own accessor
 * comments) -- returns -1 (write nothing, caller must refuse to cache
 * the whole program, exactly like before this feature existed) whenever
 * `desc`'s deferred value has no real, reparseable source span. See
 * PTAG_OP_LAZY_MARKER's own enum comment for the wire layout. */
static int write_lazy_thunk_marker(WBuf *b, void *desc) {
    unsigned int pos;
    if (!ll_lazy_thunk_descriptor_src_pos(desc, &pos)) return -1;
    const char *file_path = ll_lazy_thunk_descriptor_file_path(desc);
    const char *base_dir = ll_lazy_thunk_descriptor_base_dir(desc);
    wbuf_u8(b, PTAG_OP_LAZY_MARKER);
    wbuf_i64(b, (int64_t)pos);
    wbuf_bytes_with_len(b, file_path, strlen(file_path));
    wbuf_bytes_with_len(b, base_dir, strlen(base_dir));

    /* Collect the chain innermost-first (the only order `_up` walking
     * can produce), then emit it outermost-first (see the enum comment)
     * -- two small, bounded passes over the same (always shallow: a
     * handful of enclosing let/rec/lambda/with levels, never remotely
     * close to a real file's total node count) chain. */
    void *scope = ll_lazy_thunk_descriptor_scope(desc);
    size_t frame_count = 0;
    for (void *f = scope; f; f = ll_scope_frame_up(f)) frame_count++;
    void **frames = frame_count ? malloc(frame_count * sizeof(void *)) : NULL;
    size_t fi = 0;
    for (void *f = scope; f; f = ll_scope_frame_up(f)) frames[fi++] = f;
    wbuf_i64(b, (int64_t)frame_count);
    for (size_t i = frame_count; i-- > 0;) {
        void *f = frames[i];
        int count = ll_scope_frame_count(f);
        wbuf_i64(b, count);
        wbuf_i64(b, ll_scope_frame_is_with(f));
        wbuf_i64(b, ll_scope_frame_has_overrides(f));
        for (int ni = 0; ni < count; ni++) {
            size_t len;
            const char *name = ll_scope_frame_name(f, ni, &len);
            wbuf_bytes_with_len(b, name, len);
        }
    }
    free(frames);
    return b->failed ? -1 : 0;
}

static size_t program_op_count(IrProgram *prog) {
    size_t n = 0;
    for (; n < 10000000; n++) {
        int op_id = handler_to_op_id(prog->ops[n].handler);
        if (op_id < 0) return (size_t)-1;
        /* Lazy-thunk-compile experiment (compiler.h's own comment on
         * ll_lazy_thunk_enabled): an un-upgraded stub IrProgram's
         * ONLY op is OP_LAZY_THUNK_BODY, with no OP_RET/OP_HALT
         * terminator after it (the stub is meant to be upgraded IN
         * PLACE on first force, replacing its own ops/pool with the
         * real compiled body's -- it's never meant to run to
         * completion as-is). Without this check, this loop would walk
         * straight off the end of the stub's real 1-entry ops array
         * into whatever memory happens to follow it -- undefined
         * behavior that, before this fix, happened to usually (not
         * reliably) return a safe "unknown op" failure purely because
         * garbage bytes read as a handler pointer rarely match a real
         * table entry, not because of any actual bounds check. Explicit
         * refusal here is the correct fix regardless: this format has
         * no representation at all for a LazyThunkDescriptor's cross-
         * process state (its `body`/`scope` are compile-time-only Ast/
         * ScopeFrame pointers, meaningless in a process that didn't
         * build them) -- caching a file containing an unresolved stub
         * was never going to work, this just makes that refusal
         * explicit and bounded instead of an accidental out-of-bounds
         * read. Same observable outcome as before (ll_bytecode_
         * cache_store's own documented "refused -- skip caching"
         * contract, see that function's own comment), now via a real
         * check instead of relying on garbage memory not matching. */
        if (op_id == OP_LAZY_THUNK_BODY) return (size_t)-1;
        if (op_id == OP_HALT || op_id == OP_RET) return n + 1;
    }
    return (size_t)-1;
}

static int write_program(WBuf *b, IrProgram *prog) {
    size_t n_ops = program_op_count(prog);
    if (n_ops == (size_t)-1) return -1;

    /* Pass 1: mark op indices consumed by a following OP_MK_ATTRS's
     * grouped (name, value) pairs, so the main walk below skips emitting
     * them individually (they're emitted as part of the group instead). */
    uint8_t *consumed = calloc(n_ops, 1);
    if (!consumed) return -1;
    int ok = 1;
    for (size_t i = 0; i < n_ops && ok; i++) {
        int op_id = handler_to_op_id(prog->ops[i].handler);
        if (op_id == OP_MK_ATTRS) {
            int64_t count = prog->ops[i].a;
            if (count < 0 || (size_t)count > i) {
                ok = 0;
                break;
            }
            for (int64_t k = 0; k < count; k++) consumed[i - 1 - (size_t)k] = 1;
        }
    }
    if (!ok) {
        free(consumed);
        return -1;
    }

    wbuf_i64(b, (int64_t)n_ops);
    int rc = 0;
    for (size_t i = 0; i < n_ops && rc == 0;) {
        if (consumed[i]) {
            i++;
            continue;
        }
        int op_id = handler_to_op_id(prog->ops[i].handler);
        if (op_id == OP_MK_ATTRS) {
            int64_t count = prog->ops[i].a;
            int64_t pool_b = prog->ops[i].b;
            size_t start = i - (size_t)count;
            wbuf_u8(b, PTAG_OP_ATTRS);
            wbuf_i64(b, count);
            for (int64_t k = 0; k < count && rc == 0; k++) {
                IrOp *valop = &prog->ops[start + (size_t)k];
                uint32_t sym = (uint32_t)prog->pool[pool_b + k];
                size_t name_len;
                const char *name = ll_sym_name(sym, &name_len);
                wbuf_bytes_with_len(b, name, name_len);
                /* Each pair's value-op is written as a length-prefixed,
                 * self-contained blob (not inline) so the reader can
                 * skip/reorder pairs by byte length alone, with no need
                 * to parse-then-discard one just to find where the next
                 * starts (which would otherwise leak any nested child
                 * IrProgram that throwaway parse allocates -- found
                 * while writing read_one_tagged_op's PTAG_OP_ATTRS case
                 * below). */
                WBuf sub = { 0 };
                rc = write_one_op(&sub, valop, prog->pool);
                if (rc == 0 && !sub.failed) {
                    wbuf_i64(b, (int64_t)sub.len);
                    wbuf_bytes(b, sub.buf, sub.len);
                } else {
                    rc = -1;
                }
                free(sub.buf);
            }
            i++;
            continue;
        }
        rc = write_one_op(b, &prog->ops[i], prog->pool);
        i++;
    }
    free(consumed);
    return (rc == 0 && !b->failed) ? 0 : -1;
}

static int write_one_op(WBuf *b, IrOp *op, int64_t *pool) {
    int op_id = handler_to_op_id(op->handler);
    if (op_id < 0) return -1;
    switch (op_id) {
        case OP_PUSH_INT: {
            /* pool[op->b] is a raw (untagged) int64 literal, not a
             * fully-formed Value -- write it directly, no write_value
             * envelope (that function's encoding is for already-tagged
             * Value.raw words, e.g. OP_PUSH_CONST's string/etc. constants). */
            wbuf_u8(b, PTAG_OP_PUSHINT);
            wbuf_i64(b, pool[op->b]);
            return 0;
        }
        case OP_PUSH_CONST: {
            Value v = { .raw = (uint64_t)pool[op->b] };
            wbuf_u8(b, PTAG_OP_CONST);
            wbuf_i64(b, op->a);
            return write_value(b, v);
        }
        case OP_MK_THUNK: {
            /* pool[op->b] is now a single IrProgram* (ib_mk_thunk
             * pushes one pool slot, not a {ops,pool} pair) -- dereference
             * it directly instead of reading two separate pool slots. */
            IrProgram *child = (IrProgram *)(intptr_t)pool[op->b];
            /* Lazy-thunk + bytecode-cache reconciliation: a not-yet-
             * forced child whose ONLY op is OP_LAZY_THUNK_BODY is a
             * stub (compiler.c's emit_attrset_field_value), not an
             * ordinary compiled program -- program_op_count would
             * refuse it (and, by propagation, this whole top-level
             * write_program call) outright; try the portable marker
             * encoding first instead. write_lazy_thunk_marker itself
             * falls back to -1 (same refusal as before) whenever the
             * stub's own deferred value has no real reparseable source
             * span -- see that function's own comment. */
            if (handler_to_op_id(child->ops[0].handler) == OP_LAZY_THUNK_BODY) {
                void *desc = (void *)(intptr_t)child->pool[child->ops[0].b];
                return write_lazy_thunk_marker(b, desc);
            }
            wbuf_u8(b, PTAG_OP_THUNK);
            return write_program(b, child);
        }
        case OP_LAMBDA: {
            /* Same single-IrProgram*-slot layout as OP_MK_THUNK
             * above; formals moved from pool[op->b+2] to pool[op->b+1]
             * since the child program no longer occupies two slots. */
            IrProgram *child = (IrProgram *)(intptr_t)pool[op->b];
            Value formals = { .raw = (uint64_t)pool[op->b + 1] };
            wbuf_u8(b, PTAG_OP_LAMBDA);
            wbuf_i64(b, op->a); /* arity */
            if (write_program(b, child) != 0) return -1;
            return write_value(b, formals);
        }
        case OP_ASSERT_FAIL: {
            const char *msg = (const char *)(intptr_t)pool[op->b];
            int64_t msg_len = pool[op->b + 1];
            if (msg_len < 0) return -1;
            wbuf_u8(b, PTAG_OP_ASSERT);
            wbuf_bytes_with_len(b, msg, (size_t)msg_len);
            return 0;
        }
        case OP_CHECK_FORMALS: {
            int64_t required_count = op->a;
            int64_t pool_b = op->b;
            int64_t total_count = pool[pool_b];
            if (required_count < 0 || total_count < -1) return -1;
            wbuf_u8(b, PTAG_OP_CHECKF);
            wbuf_i64(b, required_count);
            wbuf_i64(b, total_count);
            for (int64_t i = 0; i < required_count; i++) {
                uint32_t sym = (uint32_t)pool[pool_b + 1 + i];
                size_t len;
                const char *name = ll_sym_name(sym, &len);
                wbuf_bytes_with_len(b, name, len);
            }
            if (total_count >= 0) {
                for (int64_t i = 0; i < total_count; i++) {
                    uint32_t sym = (uint32_t)pool[pool_b + 1 + required_count + i];
                    size_t len;
                    const char *name = ll_sym_name(sym, &len);
                    wbuf_bytes_with_len(b, name, len);
                }
            }
            return 0;
        }
        case OP_REC_OVERRIDES_MERGE: {
            int64_t n = op->a;
            int64_t pool_b = op->b;
            if (n < 0) return -1;
            wbuf_u8(b, PTAG_OP_RECOVR);
            wbuf_i64(b, n);
            for (int64_t i = 0; i < n; i++) {
                uint32_t sym = (uint32_t)pool[pool_b + i];
                size_t len;
                const char *name = ll_sym_name(sym, &len);
                wbuf_bytes_with_len(b, name, len);
            }
            wbuf_i64(b, pool[pool_b + n]); /* overrides_slot: plain int, not a symbol */
            return 0;
        }
        case OP_MK_ATTRS:
            /* Never reached directly -- write_program's own walk always
             * intercepts OP_MK_ATTRS itself (emitting PTAG_OP_ATTRS) and
             * marks its `count` preceding ops `consumed` so they never
             * reach this function individually either. */
            return -1;
        case OP_SELECT:
        case OP_HAS_ATTR: {
            /* `a` is a bare symbol id, not a pool index -- see this
             * file's PTAG_OP_SYM_A comment for why this needs its own
             * case (found via a real failure, not by inspection alone). */
            size_t len;
            const char *name = ll_sym_name((uint32_t)op->a, &len);
            wbuf_u8(b, PTAG_OP_SYM_A);
            wbuf_i64(b, op_id);
            wbuf_bytes_with_len(b, name, len);
            wbuf_i64(b, op->b);
            return 0;
        }
        case OP_WITH_TRY:
        case OP_SELECT_OR:
        case OP_HAS_ATTR_IF: {
            /* `b` is the bare symbol id here; `a` is a real jump delta,
             * passed through unchanged. See PTAG_OP_SYM_A's comment. */
            size_t len;
            const char *name = ll_sym_name((uint32_t)op->b, &len);
            wbuf_u8(b, PTAG_OP_SYM_B);
            wbuf_i64(b, op_id);
            wbuf_i64(b, op->a);
            wbuf_bytes_with_len(b, name, len);
            return 0;
        }
        case OP_MK_FORMALSMAP: {
            /* `ll`-port addition -- see PTAG_OP_FORMALSMAP's own comment
             * for why this needs a dedicated case at all (asnix has no
             * equivalent op to have ever needed one). `a` = nf (formal
             * count), `b` = pool index of nf POSITIONAL entries, each
             * either a real symbol id or the -1 sentinel (irbuild.c's
             * ib_mk_formalsmap). Not reusing wbuf_bytes_with_len/
             * rcur_bytes_with_len for the entries: that helper hard-
             * rejects a negative length (rcur_bytes_with_len's own `n <
             * 0` check), so the -1 sentinel is written as a bare length
             * field with no bytes following, read back explicitly below
             * rather than through that shared helper. */
            int64_t nf = op->a;
            int64_t pool_b = op->b;
            if (nf < 0) return -1;
            wbuf_u8(b, PTAG_OP_FORMALSMAP);
            wbuf_i64(b, nf);
            for (int64_t i = 0; i < nf; i++) {
                int64_t sym_or_neg1 = pool[pool_b + i];
                if (sym_or_neg1 < 0) {
                    wbuf_i64(b, -1);
                } else {
                    size_t len;
                    const char *name = ll_sym_name((uint32_t)sym_or_neg1, &len);
                    wbuf_i64(b, (int64_t)len);
                    wbuf_bytes(b, name, len);
                }
            }
            return 0;
        }
        default:
            wbuf_u8(b, PTAG_OP);
            wbuf_i64(b, op_id);
            wbuf_i64(b, op->a);
            wbuf_i64(b, op->b);
            return 0;
    }
}

int ll_bytecode_serialize(IrProgram *prog, uint8_t **out_buf, size_t *out_len) {
    WBuf b = { 0 };
    /* The magic string MUST be bumped whenever the wire format changes
     * in a way that isn't self-describing. Two concrete reasons on
     * record: (1) the opcode/operand/length/symbol-id fields moved from
     * fixed 8-byte int64 to a variable-width zigzag+LEB128 varint, and a
     * stale fixed-width entry must never be misread as varint data (the
     * byte layout no longer lines up at all past the first field); (2)
     * OP_PUSH_INT's own wire encoding changed to PTAG_OP_PUSHINT -- an
     * older entry wrote it under plain PTAG_OP with the literal inline
     * in `a` (back when IrOp.a was 64 bits); this binary's IrOp.a is now
     * only 32 bits, so blindly replaying that old entry would SILENTLY
     * TRUNCATE any literal outside +-2^31 instead of erroring -- not a
     * crash, a wrong answer. ll_bytecode_deserialize's own magic check
     * below rejects any mismatched magic outright, same as any other
     * corrupt/foreign entry, degrading cleanly to a cache miss. */
    wbuf_bytes(&b, "ASNXBC03", 8);
    if (write_program(&b, prog) != 0 || b.failed) {
        free(b.buf);
        return -1;
    }
    *out_buf = b.buf;
    *out_len = b.len;
    return 0;
}

/* ---------------------------------------------------------------------
 * Deserialization: mirrors write_program/write_one_op exactly, emitting
 * a fresh ops[]/pool[] pair (malloc'd, growable the same way
 * irbuild.c's own ib_emit/ib_pool_add grow -- these are handed to
 * ir_link/ll_eval exactly like ib_finish's own output, so they must
 * use the SAME allocator convention, not the arena: see irbuild.c's
 * IrBuilder for precedent). Returns NULL on ANY anomaly -- no partial
 * programs are ever returned.
 * --------------------------------------------------------------------- */
typedef struct {
    IrOp *ops;
    size_t len, cap;
    int64_t *pool;
    size_t pool_len, pool_cap;
    int failed;
} DBuilder;

static void db_emit(DBuilder *d, int op_id, int64_t a, int64_t b) {
    if (d->failed) return;
    if (d->len == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 16;
        IrOp *n = realloc(d->ops, d->cap * sizeof(IrOp));
        if (!n) {
            d->failed = 1;
            return;
        }
        d->ops = n;
    }
    d->ops[d->len].handler = (void *)(intptr_t)op_id;
    d->ops[d->len].a = a;
    d->ops[d->len].b = b;
    d->len++;
}

static int64_t db_pool_add(DBuilder *d, int64_t v) {
    if (d->failed) return 0;
    if (d->pool_len == d->pool_cap) {
        d->pool_cap = d->pool_cap ? d->pool_cap * 2 : 8;
        int64_t *n = realloc(d->pool, d->pool_cap * sizeof(int64_t));
        if (!n) {
            d->failed = 1;
            return 0;
        }
        d->pool = n;
    }
    d->pool[d->pool_len] = v;
    return (int64_t)(d->pool_len++);
}

static int read_program_into(RCur *r, DBuilder *d);

/* Reads `count` length-prefixed names, re-interns each, sorts the
 * resulting ids ascending, and appends them to `d`'s pool in that sorted
 * order -- shared by OP_CHECK_FORMALS's two independently-sortable lists
 * (see that case's own comment for why this sort is necessary at all:
 * op_check_formals.S binary-searches against them, same invariant
 * OP_MK_ATTRS's own grouping logic protects, just without that op's
 * extra "stay in sync with a preceding value-push sequence" wrinkle). */
static int read_sorted_symbol_list(RCur *r, DBuilder *d, int64_t count) {
    if (count == 0) return 0;
    uint32_t *syms = malloc((size_t)count * sizeof(uint32_t));
    for (int64_t i = 0; i < count; i++) {
        size_t len;
        const char *name = rcur_bytes_with_len(r, &len);
        if (r->failed) {
            free(syms);
            return -1;
        }
        syms[i] = ll_intern(name, len);
    }
    for (int64_t i = 1; i < count; i++) {
        int64_t j = i;
        while (j > 0 && syms[j - 1] > syms[j]) {
            uint32_t t = syms[j - 1];
            syms[j - 1] = syms[j];
            syms[j] = t;
            j--;
        }
    }
    for (int64_t i = 0; i < count; i++) db_pool_add(d, (int64_t)syms[i]);
    free(syms);
    return 0;
}

static int read_one_tagged_op(RCur *r, DBuilder *d) {
    uint8_t ptag = rcur_u8(r);
    if (r->failed) return -1;
    switch (ptag) {
        case PTAG_OP: {
            int64_t op_id = rcur_i64(r);
            int64_t a = rcur_i64(r);
            int64_t b = rcur_i64(r);
            if (r->failed || op_id < 0 || op_id >= OP_COUNT) return -1;
            db_emit(d, (int)op_id, a, b);
            return d->failed ? -1 : 0;
        }
        case PTAG_OP_SYM_A: {
            int64_t op_id = rcur_i64(r);
            size_t len;
            const char *name = rcur_bytes_with_len(r, &len);
            int64_t b = rcur_i64(r);
            if (r->failed || op_id < 0 || op_id >= OP_COUNT) return -1;
            db_emit(d, (int)op_id, (int64_t)ll_intern(name, len), b);
            return d->failed ? -1 : 0;
        }
        case PTAG_OP_SYM_B: {
            int64_t op_id = rcur_i64(r);
            int64_t a = rcur_i64(r);
            size_t len;
            const char *name = rcur_bytes_with_len(r, &len);
            if (r->failed || op_id < 0 || op_id >= OP_COUNT) return -1;
            db_emit(d, (int)op_id, a, (int64_t)ll_intern(name, len));
            return d->failed ? -1 : 0;
        }
        case PTAG_OP_PUSHINT: {
            /* See write_one_op's matching OP_PUSH_INT case -- the
             * payload is a bare int64 literal, not a tagged Value. */
            int64_t v = rcur_i64(r);
            if (r->failed) return -1;
            int64_t idx = db_pool_add(d, v);
            db_emit(d, OP_PUSH_INT, 0, idx);
            return d->failed ? -1 : 0;
        }
        case PTAG_OP_CONST: {
            int64_t a = rcur_i64(r);
            Value v;
            if (r->failed || read_value(r, &v) != 0) return -1;
            int64_t idx = db_pool_add(d, (int64_t)v.raw);
            db_emit(d, OP_PUSH_CONST, a, idx);
            return d->failed ? -1 : 0;
        }
        case PTAG_OP_THUNK: {
            DBuilder child = { 0 };
            if (read_program_into(r, &child) != 0) return -1;
            /* Every nested program needs its OWN ir_link call -- only
             * the outermost one gets linked by ll_bytecode_deserialize
             * itself. Missing this leaves child.ops[i].handler holding a
             * raw op-id integer (0, 1, 2, ...) instead of a real handler
             * function pointer; op_call.S/force.S then `jmp *that`,
             * jumping to a near-NULL address the instant this thunk is
             * ever forced -- exactly the segfault this fixes (found via
             * gdb: "Program received signal SIGSEGV... 0x000000000000000c",
             * `0xc` being a small op-id, not a code address). */
            ir_link(child.ops, child.len);
            /* Thunk/Closure's runtime consumer expects ONE pool
             * slot holding an IrProgram*, not a {ops,pool} pair -- box
             * the freshly-read child the same way ib_finish does. */
            IrProgram *child_prog = malloc(sizeof(IrProgram));
            child_prog->ops = child.ops;
            child_prog->pool = child.pool;
            int64_t idx = db_pool_add(d, (int64_t)(intptr_t)child_prog);
            db_emit(d, OP_MK_THUNK, 0, idx);
            return d->failed ? -1 : 0;
        }
        case PTAG_OP_LAMBDA: {
            int64_t arity = rcur_i64(r);
            DBuilder child = { 0 };
            if (r->failed || read_program_into(r, &child) != 0) return -1;
            ir_link(child.ops, child.len); /* see PTAG_OP_THUNK's identical comment above */
            Value formals;
            if (read_value(r, &formals) != 0) return -1;
            IrProgram *child_prog = malloc(sizeof(IrProgram));
            child_prog->ops = child.ops;
            child_prog->pool = child.pool;
            int64_t idx = db_pool_add(d, (int64_t)(intptr_t)child_prog);
            db_pool_add(d, (int64_t)formals.raw);
            db_emit(d, OP_LAMBDA, arity, idx);
            return d->failed ? -1 : 0;
        }
        case PTAG_OP_ASSERT: {
            size_t len;
            const char *msg = rcur_bytes_with_len(r, &len);
            if (r->failed) return -1;
            int64_t idx = db_pool_add(d, (int64_t)(intptr_t)msg);
            db_pool_add(d, (int64_t)len);
            db_emit(d, OP_ASSERT_FAIL, 0, idx);
            return d->failed ? -1 : 0;
        }
        case PTAG_OP_CHECKF: {
            int64_t required_count = rcur_i64(r);
            int64_t total_count = rcur_i64(r);
            if (r->failed || required_count < 0 || total_count < -1 || required_count > 1000000 ||
                total_count > 1000000)
                return -1;
            int64_t idx = db_pool_add(d, total_count);
            /* Both lists are "sorted ascending by symbol id" per
             * op_check_formals.S's own header comment (it binary-
             * searches the given Attrs against them) -- re-interning
             * does not preserve that order (ids are assigned by first-
             * seen order per process), so each list is independently
             * re-sorted by its own freshly interned ids before being
             * written into the pool, exactly the same bug class
             * OP_MK_ATTRS's own grouping fix addresses (found the same
             * way: a real "unexpected argument" failure on a cached
             * `{ lib, stdenv, callPackage, ... }`-shaped lambda, not by
             * inspection alone). */
            if (read_sorted_symbol_list(r, d, required_count) != 0) return -1;
            if (total_count >= 0 && read_sorted_symbol_list(r, d, total_count) != 0) return -1;
            db_emit(d, OP_CHECK_FORMALS, required_count, idx);
            return d->failed ? -1 : 0;
        }
        case PTAG_OP_RECOVR: {
            int64_t n = rcur_i64(r);
            if (r->failed || n < 0) return -1;
            int64_t idx = -1;
            for (int64_t i = 0; i < n; i++) {
                size_t len;
                const char *name = rcur_bytes_with_len(r, &len);
                if (r->failed) return -1;
                int64_t pidx = db_pool_add(d, (int64_t)ll_intern(name, len));
                if (i == 0) idx = pidx;
            }
            int64_t overrides_slot = rcur_i64(r);
            if (r->failed) return -1;
            int64_t slot_idx = db_pool_add(d, overrides_slot);
            if (n == 0) idx = slot_idx;
            db_emit(d, OP_REC_OVERRIDES_MERGE, n, idx);
            return d->failed ? -1 : 0;
        }
        case PTAG_OP_FORMALSMAP: {
            /* `ll`-port addition -- mirrors write_one_op's OP_MK_FORMALSMAP
             * case exactly. Each entry's length field doubles as the -1
             * sentinel (a formal with a default, not covered by this
             * mechanism) -- read directly via rcur_i64, NOT through
             * rcur_bytes_with_len (which hard-rejects a negative length),
             * since -1 is a valid, expected value here, not corruption. */
            int64_t nf = rcur_i64(r);
            if (r->failed || nf < 0 || nf > 1000000) return -1;
            int64_t idx = -1;
            for (int64_t i = 0; i < nf; i++) {
                int64_t len = rcur_i64(r);
                int64_t pidx;
                if (r->failed) return -1;
                if (len < 0) {
                    pidx = db_pool_add(d, -1);
                } else {
                    if ((size_t)len > r->len - r->pos) return -1;
                    char *name = malloc((size_t)len > 0 ? (size_t)len : 1);
                    rcur_bytes(r, name, (size_t)len);
                    if (r->failed) {
                        free(name);
                        return -1;
                    }
                    pidx = db_pool_add(d, (int64_t)ll_intern(name, (size_t)len));
                    free(name);
                }
                if (i == 0) idx = pidx;
            }
            if (nf == 0) idx = db_pool_add(d, 0); /* keep `b` a valid pool index even for
                                                    * a (vanishingly rare) zero-formal case */
            db_emit(d, OP_MK_FORMALSMAP, nf, idx);
            return d->failed ? -1 : 0;
        }
        case PTAG_OP_LAZY_MARKER: {
            /* Mirrors PTAG_OP_THUNK's own shape (a self-contained
             * one-op child program, boxed into its own IrProgram*, then
             * referenced by an OP_MK_THUNK emitted into the PARENT `d`)
             * -- see that case's own "every nested program needs its
             * OWN ir_link call" comment, which applies identically
             * here: the stub's single op must go through ir_link before
             * anything ever dispatches through it, exactly like the
             * live path's own ib_mk_lazy_thunk_stub/ib_finish output
             * does (that one gets linked later, as part of whatever
             * encloses it; this one needs it done explicitly, right
             * here, since nothing else will). */
            int64_t pos = rcur_i64(r);
            size_t fp_len, bd_len;
            const char *fp = rcur_bytes_with_len(r, &fp_len);
            const char *bd = rcur_bytes_with_len(r, &bd_len);
            int64_t frame_count = rcur_i64(r);
            if (r->failed || pos < 0 || pos > 0xFFFFFFFFLL || frame_count < 0 || frame_count > 1000000)
                return -1;
            char *file_path = malloc(fp_len + 1);
            memcpy(file_path, fp, fp_len);
            file_path[fp_len] = '\0';
            char *base_dir = malloc(bd_len + 1);
            memcpy(base_dir, bd, bd_len);
            base_dir[bd_len] = '\0';

            void *scope_chain = NULL;
            for (int64_t fi = 0; fi < frame_count; fi++) {
                int64_t count = rcur_i64(r);
                int64_t is_with = rcur_i64(r);
                int64_t has_overrides = rcur_i64(r);
                if (r->failed || count < 0 || count > 1000000) return -1;
                const char **names = count > 0 ? malloc((size_t)count * sizeof(char *)) : NULL;
                size_t *lens = count > 0 ? malloc((size_t)count * sizeof(size_t)) : NULL;
                int ok = 1;
                for (int64_t ni = 0; ni < count; ni++) {
                    size_t nlen;
                    const char *nm = rcur_bytes_with_len(r, &nlen);
                    if (r->failed) {
                        ok = 0;
                        break;
                    }
                    names[ni] = nm;
                    lens[ni] = nlen;
                }
                if (ok) scope_chain = ll_lazy_scope_push_frame(scope_chain, (int)count, (int)is_with,
                                                                   (int)has_overrides, names, lens);
                free(names);
                free(lens);
                if (!ok) return -1;
            }

            void *desc = ll_make_cached_lazy_thunk_descriptor((unsigned int)pos, file_path, base_dir,
                                                                  scope_chain);
            DBuilder stub = { 0 };
            int64_t desc_idx = db_pool_add(&stub, (int64_t)(intptr_t)desc);
            db_emit(&stub, OP_LAZY_THUNK_BODY, 0, desc_idx);
            if (stub.failed) return -1;
            ir_link(stub.ops, stub.len);
            IrProgram *stub_prog = malloc(sizeof(IrProgram));
            stub_prog->ops = stub.ops;
            stub_prog->pool = stub.pool;
            ll_lazy_thunk_descriptor_set_self(desc, stub_prog);

            int64_t idx = db_pool_add(d, (int64_t)(intptr_t)stub_prog);
            db_emit(d, OP_MK_THUNK, 0, idx);
            return d->failed ? -1 : 0;
        }
        case PTAG_OP_ATTRS: {
            int64_t count = rcur_i64(r);
            if (r->failed || count < 0 || count > 1000000) return -1;
            uint32_t *new_syms = count > 0 ? malloc((size_t)count * sizeof(uint32_t)) : NULL;
            size_t *order = count > 0 ? malloc((size_t)count * sizeof(size_t)) : NULL;
            /* Each pair's value-op was written as a length-prefixed,
             * self-contained blob (see write_program's OP_MK_ATTRS
             * case), so finding where each one starts is a pure byte-
             * length skip -- no need to parse-then-discard a pair just
             * to find the next one's offset (which would otherwise leak
             * any nested child IrProgram that throwaway parse
             * allocates). */
            size_t *blob_off = count > 0 ? malloc((size_t)count * sizeof(size_t)) : NULL;
            size_t *blob_len = count > 0 ? malloc((size_t)count * sizeof(size_t)) : NULL;
            for (int64_t i = 0; i < count; i++) {
                size_t len;
                const char *name = rcur_bytes_with_len(r, &len);
                if (r->failed) {
                    free(new_syms);
                    free(order);
                    free(blob_off);
                    free(blob_len);
                    return -1;
                }
                new_syms[i] = ll_intern(name, len);
                int64_t bl = rcur_i64(r);
                if (r->failed || bl < 0 || (size_t)bl > r->len - r->pos) {
                    free(new_syms);
                    free(order);
                    free(blob_off);
                    free(blob_len);
                    return -1;
                }
                blob_off[i] = r->pos;
                blob_len[i] = (size_t)bl;
                r->pos += (size_t)bl;
                order[i] = (size_t)i;
            }
            /* Sort `order` by new_syms ascending -- re-establishing the
             * sorted-by-symbol-id invariant OP_MK_ATTRS's own asm
             * handler trusts without checking (see this file's header
             * comment on why this step exists at all). Small counts
             * (real attrsets are rarely more than a few dozen bindings)
             * make insertion sort fine, same simplicity call
             * ll_bshared_make_attrs itself already makes. */
            for (size_t i = 1; i < (size_t)count; i++) {
                size_t j = i;
                while (j > 0 && new_syms[order[j - 1]] > new_syms[order[j]]) {
                    size_t t = order[j - 1];
                    order[j - 1] = order[j];
                    order[j] = t;
                    j--;
                }
            }
            int rc = 0;
            int64_t pool_start = -1;
            for (int64_t i = 0; i < count && rc == 0; i++) {
                size_t slot = order[i];
                RCur sub = { .buf = r->buf, .len = blob_off[slot] + blob_len[slot], .pos = blob_off[slot], .failed = 0 };
                if (read_one_tagged_op(&sub, d) != 0 || sub.pos != sub.len) rc = -1;
            }
            if (rc == 0) {
                for (int64_t i = 0; i < count; i++) {
                    int64_t pidx = db_pool_add(d, (int64_t)new_syms[order[i]]);
                    if (i == 0) pool_start = pidx;
                }
                if (count == 0) pool_start = db_pool_add(d, 0);
                db_emit(d, OP_MK_ATTRS, count, pool_start);
            }
            free(new_syms);
            free(order);
            free(blob_off);
            free(blob_len);
            return (rc == 0 && !d->failed) ? 0 : -1;
        }
        default:
            return -1;
    }
}

static int read_program_into(RCur *r, DBuilder *d) {
    int64_t n_ops = rcur_i64(r);
    if (r->failed || n_ops <= 0 || n_ops > 10000000) return -1;
    /* `n_ops` is this level's EXACT final `d->len`: write_program's own
     * counting (program_op_count) runs on the ORIGINAL uncollapsed op
     * array before OP_MK_ATTRS's grouped pairs are folded into one
     * PTAG_OP_ATTRS entry, and read_program_into's own loop below
     * advances `i` by however many ops a single tagged entry actually
     * emits (PTAG_OP_ATTRS emits `count+1` for one group entry, every
     * other tag emits exactly 1) -- so by the time this loop finishes,
     * `d->len` always equals `n_ops` exactly. Presizing `d->ops` to
     * `n_ops` up front (this DBuilder is always freshly `{0}`-initialized
     * by every call site) replaces db_emit's usual start-at-16-and-double
     * growth with a single allocation and zero growth-reallocs for `ops`
     * at this level. `pool` has no equivalent exact count (OP_CHECK_
     * FORMALS/OP_REC_OVERRIDES_MERGE can add many pool entries for a
     * single op), so it's only a heuristic starting guess here -- still
     * correctly grows via the ordinary doubling path in db_pool_add if
     * exceeded. */
    if (d->cap == 0) {
        d->ops = malloc((size_t)n_ops * sizeof(IrOp));
        if (!d->ops) return -1;
        d->cap = (size_t)n_ops;
    }
    if (d->pool_cap == 0) {
        d->pool = malloc((size_t)n_ops * sizeof(int64_t));
        if (!d->pool) return -1;
        d->pool_cap = (size_t)n_ops;
    }
    int64_t emitted_before;
    for (int64_t i = 0; i < n_ops;) {
        emitted_before = (int64_t)d->len;
        if (read_one_tagged_op(r, d) != 0) return -1;
        i += (int64_t)d->len - emitted_before; /* PTAG_OP_ATTRS can net-emit >1 op per group entry */
    }
    return d->failed ? -1 : 0;
}

IrProgram *ll_bytecode_deserialize(const uint8_t *buf, size_t len) {
    /* Also rejects any entry written under an older magic (an earlier
     * fixed-8-byte-int wire format, or OP_PUSH_INT's old inline-`a`
     * encoding) -- see ll_bytecode_serialize's own comment on the
     * magic bump. */
    if (len < 8 || memcmp(buf, "ASNXBC03", 8) != 0) return NULL;
    RCur r = { .buf = buf, .len = len, .pos = 8, .failed = 0 };
    DBuilder d = { 0 };
    if (read_program_into(&r, &d) != 0 || r.pos != len) {
        free(d.ops);
        free(d.pool);
        return NULL;
    }
    ir_link(d.ops, d.len);
    IrProgram *prog = malloc(sizeof(IrProgram));
    prog->ops = d.ops;
    prog->pool = d.pool;
    return prog;
}
