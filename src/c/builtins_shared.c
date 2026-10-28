/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "builtins_shared.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/env.h"
#include "asnix/heap.h"
#include "asnix/stats.h"
#include "asnix/ir.h"
#include "asnix/irbuild.h"
#include "symtab.h"
#include "value_util.h"

Value asnix_bshared_make_primop(void *fn, int64_t arity) {
    void *mem = asnix_arena_alloc(ASNIX_PRIMOP_SIZE);
    asnix_stats_primop++;
    *(void **)((char *)mem + ASNIX_PRIMOP_OFF_FN) = fn;
    *(int64_t *)((char *)mem + ASNIX_PRIMOP_OFF_ARITY) = arity;
    /* ASNIX_PRIMOP_OFF_NOFORCE left at its fresh-arena zero (force args,
     * the default for every primop except tryEval -- see that field's
     * own comment, heap.h). */
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_PRIMOP };
}

Value asnix_bshared_make_primop_noforce(void *fn, int64_t arity) {
    void *mem = asnix_arena_alloc(ASNIX_PRIMOP_SIZE);
    asnix_stats_primop++;
    *(void **)((char *)mem + ASNIX_PRIMOP_OFF_FN) = fn;
    *(int64_t *)((char *)mem + ASNIX_PRIMOP_OFF_ARITY) = arity;
    *(int64_t *)((char *)mem + ASNIX_PRIMOP_OFF_NOFORCE) = 1;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_PRIMOP };
}

/* M80: the ONE canonical marker Value used by every native-builtin-
 * currying helper in this codebase -- see builtins_shared.h's own long
 * comment on the declaration for the full rationale. Lazily built on
 * first use (happens very early, during the one-time builtin-table
 * registration pass that runs before any Nix source is even parsed, let
 * alone compiled) and memoized in a function-local static, the same
 * "compute once, cache in a static" shape this codebase already uses
 * elsewhere (e.g. builtins_attrs.c's prim_hasAttr/prim_getAttr `static
 * IrProgram *inner`). */
Value asnix_bshared_native_formals_marker(void) {
    static Value marker = { .raw = 0 };
    if (marker.raw == 0) marker = asnix_bshared_make_attrs(NULL, NULL, 0);
    return marker;
}

/* Ported straight from builtins_misc.c's make_curried_native -- see that
 * file's long header comment for the full rationale (equivalent to
 * compiling the fictitious Nix source `a1: a2: ... aN: __native a1 a2 ... aN`
 * via the same IrBuilder API the real compiler uses for ordinary curried
 * lambdas). */
Value asnix_bshared_make_curried_native(void *native_fn, int64_t arity) {
    Value native_primop = asnix_bshared_make_primop(native_fn, arity);
    /* Never a lambda-pattern -- `builtins.functionArgs` on any of these
     * closures is `{}`, same as real Nix's `functionArgs builtins.add`
     * (M37). M80: uses the shared canonical marker (see above) instead of
     * a fresh `asnix_bshared_make_attrs` call so the printer can
     * recognize these as native-builtin curry stages (still a completely
     * valid, correct `{ }` for `functionArgs` itself). */
    Value empty_formals = asnix_bshared_native_formals_marker();

    IrBuilder *b = ib_new();
    ib_push_const(b, (int64_t)native_primop.raw);
    for (int64_t depth = arity - 1; depth >= 0; depth--) ib_var_ref(b, depth, 0);
    ib_call(b, arity);
    ib_ret(b);
    IrProgram *prog = ib_finish(b);

    for (int64_t i = arity - 1; i >= 1; i--) {
        IrBuilder *wrap_b = ib_new();
        ib_lambda(wrap_b, 1, prog, (int64_t)empty_formals.raw);
        ib_ret(wrap_b);
        prog = ib_finish(wrap_b);
    }

    void *mem = asnix_arena_alloc(ASNIX_CLOSURE_SIZE);
    asnix_stats_closure++;
    *(void **)((char *)mem + ASNIX_CLOSURE_OFF_UP) = NULL;
    *(IrProgram **)((char *)mem + ASNIX_CLOSURE_OFF_PROG) = prog; /* M100: one pointer, not {ops,pool} */
    *(int64_t *)((char *)mem + ASNIX_CLOSURE_OFF_ARITY) = 1;
    *(uint64_t *)((char *)mem + ASNIX_CLOSURE_OFF_FORMALS) = empty_formals.raw;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_CLOSURE };
}

/* M92: see builtins_shared.h's own long comment -- identical to
 * asnix_bshared_make_curried_native just above, except the reassembly
 * loop uses ib_var_ref_raw (no forcing at all) instead of ib_var_ref,
 * and the leaf primop is built NOFORCE (asnix_bshared_make_primop_
 * noforce) instead of force-by-default. */
Value asnix_bshared_make_curried_native_noforce(void *native_fn, int64_t arity) {
    asnix_stats_closure++;
    Value native_primop = asnix_bshared_make_primop_noforce(native_fn, arity);
    Value empty_formals = asnix_bshared_native_formals_marker();

    IrBuilder *b = ib_new();
    ib_push_const(b, (int64_t)native_primop.raw);
    for (int64_t depth = arity - 1; depth >= 0; depth--) ib_var_ref_raw(b, depth, 0);
    ib_call(b, arity);
    ib_ret(b);
    IrProgram *prog = ib_finish(b);

    for (int64_t i = arity - 1; i >= 1; i--) {
        IrBuilder *wrap_b = ib_new();
        ib_lambda(wrap_b, 1, prog, (int64_t)empty_formals.raw);
        ib_ret(wrap_b);
        prog = ib_finish(wrap_b);
    }

    void *mem = asnix_arena_alloc(ASNIX_CLOSURE_SIZE);
    *(void **)((char *)mem + ASNIX_CLOSURE_OFF_UP) = NULL;
    *(IrProgram **)((char *)mem + ASNIX_CLOSURE_OFF_PROG) = prog; /* M100: one pointer, not {ops,pool} */
    *(int64_t *)((char *)mem + ASNIX_CLOSURE_OFF_ARITY) = 1;
    *(uint64_t *)((char *)mem + ASNIX_CLOSURE_OFF_FORMALS) = empty_formals.raw;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_CLOSURE };
}

Value asnix_bshared_apply2(Value fn, Value a, Value b) {
    Value step1 = asnix_apply(fn, &a, 1);
    return asnix_apply(step1, &b, 1);
}

void asnix_bshared_fatal(const char *who, const char *msg) {
    fprintf(stderr, "asnix: %s: %s\n", who, msg);
    exit(1);
}

/* OP_SELECT's (op_select.S) static-attribute-missing fatal path -- names
 * the attribute, matching real Nix's own "attribute 'x' missing" wording
 * (confirmed via `nix-instantiate --eval '{ a = 1; }.b'`). Previously a
 * single generic "attribute missing, aborting" line with no indication of
 * *which* attribute or selection site -- found to cost real debugging time
 * (every "attribute missing" abort chasing real nixpkgs needed a one-off
 * gdb session reading the symbol id out of the live IP register to find
 * out). Only ever called immediately before `exit(1)`, so the extra
 * `asnix_sym_name` call/format here costs nothing on the hot (successful
 * select) path -- op_select.S's own `.Lselect_found`/`.Lselect_push`
 * never reach this function at all. */
_Noreturn void asnix_bshared_select_missing_fatal(uint32_t sym) {
    size_t len;
    const char *name = asnix_sym_name(sym, &len);
    fprintf(stderr, "asnix: attribute '%.*s' missing, aborting\n", (int)len, name);
    exit(1);
}

/* M50: the "cheap pass" extension of M36's idea -- name the actual VALUE
 * (its type/kind) at every other fatal path that previously said nothing
 * more specific than "is not an attrset"/"is not a function". Reuses the
 * exact same tag -> name mapping builtins.c's `prim_typeOf` already
 * established (int/bool/null/string/path/list/set/lambda), just phrased
 * as a full "a/an X" clause instead of typeOf's bare noun, since every
 * call site below wants to say "...(got X), aborting". Deliberately does
 * NOT force `v` first (unlike typeOf, which relies on OP_CALL having
 * already forced every primop argument, M25): every call site below reads
 * a value already sitting on VALSTACK or passed in from an asm op's own
 * C glue, which by this evaluator's standing invariant (op_call.S's own
 * M23 comment: every value reaching VALSTACK is already forced) is never
 * an unforced Thunk in practice -- the ASNIX_TAG_THUNK case is dead code,
 * kept only so a future violation of that invariant prints something sane
 * instead of misreading raw thunk bits. Declared non-static (in
 * builtins_shared.h) because attrs_update.c's asnix_attrs_insert_dynamic
 * needs it too, not just this file's own dynamic-select fatals below. */
const char *asnix_bshared_type_name(Value v) {
    switch (asnix_tag_of(v)) {
        case ASNIX_TAG_INT: return "an integer";
        case ASNIX_TAG_IMM:
            switch (asnix_imm_subtag(v)) {
                case ASNIX_IMM_TRUE:
                case ASNIX_IMM_FALSE: return "a boolean";
                case ASNIX_IMM_NULL: return "null";
                default: return "an immediate value";
            }
        case ASNIX_TAG_PTR:
            return asnix_value_is_path(v) ? "a path" : asnix_value_is_float(v) ? "a float" : "a string";
        case ASNIX_TAG_LIST: return "a list";
        case ASNIX_TAG_ATTRS: return "an attrset";
        case ASNIX_TAG_CLOSURE:
        case ASNIX_TAG_PRIMOP: return "a function";
        case ASNIX_TAG_THUNK: return "an unforced thunk"; /* see comment above: dead in practice */
        default: return "a value of unknown type";
    }
}

/* OP_SELECT's (op_select.S) type-error fatal path -- names the actual
 * type of the non-attrset select target, same motivation as
 * asnix_bshared_select_missing_fatal above (M36) applied to the sibling
 * fatal one op up: "asnix: '.' selection target is not an attrset,
 * aborting" gave no indication of what the value actually WAS, forcing
 * the same kind of one-off gdb session M36's own commit message
 * complains about. `v` arrives still tagged (the raw Value popped off
 * VALSTACK, before op_select.S's own `andq $-8` untagging) -- exactly
 * what asnix_bshared_type_name above wants. */
_Noreturn void asnix_bshared_select_type_fatal(Value v) {
    fprintf(stderr, "asnix: '.' selection target is not an attrset (got %s), aborting\n",
            asnix_bshared_type_name(v));
    exit(1);
}

/* OP_CALL's (op_call.S) "call target is not a function" fatal path --
 * same idea, one op over: names the actual type of whatever was called.
 * `v` arrives still tagged (read out of the callee's VALSTACK slot
 * before op_call.S's own closure/primop dispatch, which is exactly the
 * point this path is reached from -- neither tag matched). */
_Noreturn void asnix_bshared_call_type_fatal(Value v) {
    fprintf(stderr, "asnix: call target is not a function (got %s), aborting\n",
            asnix_bshared_type_name(v));
    exit(1);
}

/* `__functor` protocol (M42): calling an attrset `s` with argument `arg`
 * (`s arg`) is sugar for `s.__functor s arg` when `s` has a `__functor`
 * attribute -- real Nix's exact mechanism, confirmed in
 * `~/nix/src/libexpr/eval.cc`'s `callFunction`: `callFunction(functor,
 * {self_copy, args[0]}, ...)`, i.e. `__functor` is called with the
 * attrset itself as its first argument, then the real argument second
 * (a curried 2-arg function in Nix terms, which this project already
 * represents as two nested arity-1 Closures, so this is just two
 * ordinary `asnix_apply` calls chained). Reached from both `OP_CALL`
 * (op_call.S) and `asnix_apply` (apply.S) ONLY after their own Closure/
 * PrimOp dispatch has already failed to match -- so this never costs
 * anything on the common (plain function call) path. Does not special-
 * case a `__functor` that itself returns another attrset-with-
 * `__functor`; `asnix_apply`'s own ATTRS branch (which this function's
 * second `asnix_apply` call re-enters) already handles that
 * recursively for free, matching real Nix's own `while` loop over
 * `args` in `callFunction`, which re-checks `vCur`'s type after every
 * step. Fatal (same "not a function" message as a plain attrset) if
 * `attrs_val` has no `__functor` attribute at all -- confirmed against
 * real Nix: `{ a = 1; } 5` and `{ __functor = ...; }`-less attrsets
 * both get the identical "attempt to call something which is not a
 * function" error. */
Value asnix_bshared_call_functor(Value attrs_val, Value arg) {
    uint32_t sym = asnix_intern("__functor", 9);
    int64_t idx = asnix_bshared_attrs_find(attrs_val, sym);
    if (idx < 0) asnix_bshared_call_type_fatal(attrs_val);
    Value functor_fn = asnix_force_for_print(asnix_bshared_attrs_value(attrs_val, idx));
    Value step1 = asnix_apply(functor_fn, &attrs_val, 1);
    return asnix_apply(step1, &arg, 1);
}

/* OP_CHECK_FORMALS's (op_check_formals.S) two lambda-pattern-mismatch
 * fatal paths -- same idea as the attribute-missing case above (M36),
 * applied to `{a, b, ...}: ...`'s own argument-validation errors: neither
 * "lambda called without a required argument" nor "lambda called with an
 * unexpected argument" said *which* field, which is exactly the
 * information a human needs to fix the call site. `sym` arrives already
 * resolved out of the IR constant pool (missing case, op_check_formals.S's
 * %r15) or out of the given attrset's own entries (extra case, %r11) --
 * both already-interned symbol ids, same asnix_sym_name lookup M36 used. */
_Noreturn void asnix_bshared_cf_missing_fatal(uint32_t sym) {
    size_t len;
    const char *name = asnix_sym_name(sym, &len);
    fprintf(stderr, "asnix: lambda called without a required argument '%.*s', aborting\n", (int)len, name);
    exit(1);
}

_Noreturn void asnix_bshared_cf_extra_fatal(uint32_t sym) {
    size_t len;
    const char *name = asnix_sym_name(sym, &len);
    fprintf(stderr, "asnix: lambda called with an unexpected argument '%.*s', aborting\n", (int)len, name);
    exit(1);
}

void asnix_bshared_require_tag(Value v, int tag, const char *who, const char *what) {
    if (asnix_tag_of(v) != tag) {
        fprintf(stderr, "asnix: %s: argument is not %s\n", who, what);
        exit(1);
    }
}

int64_t asnix_bshared_list_count(Value v) {
    return *(const int64_t *)((const char *)(uintptr_t)(v.raw & ~(uint64_t)ASNIX_TAG_MASK) + ASNIX_LIST_OFF_COUNT);
}

const Value *asnix_bshared_list_elems(Value v) {
    return (const Value *)((const char *)(uintptr_t)(v.raw & ~(uint64_t)ASNIX_TAG_MASK) + ASNIX_LIST_OFF_ELEMS);
}

Value asnix_bshared_make_list(const Value *elems, int64_t count) {
    void *mem = asnix_arena_alloc(8 + (uint64_t)count * sizeof(Value));
    asnix_stats_list++;
    asnix_stats_list_elements += (uint64_t)count;
    *(int64_t *)((char *)mem + ASNIX_LIST_OFF_COUNT) = count;
    if (count > 0) memcpy((char *)mem + ASNIX_LIST_OFF_ELEMS, elems, (size_t)count * sizeof(Value));
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_LIST };
}

/* M-garbage-fix: merges two already-flat (sym-sorted) attrsets' entry
 * arrays into a caller-supplied output buffer, preserving `//`'s
 * right-biased collision semantics (rhs wins on a duplicate key). This is
 * the EXACT SAME two-pointer comparison/copy logic as
 * asnix_attrs_eager_merge's own inner loop (attrs_update.c) -- duplicated
 * here deliberately, not shared, because this helper is parameterized
 * over raw (entries pointer, count) pairs and a buffer the CALLER already
 * owns, instead of allocating (and stats-counting) its own output the way
 * asnix_attrs_eager_merge must keep doing for its own, unrelated call
 * sites. Used ONLY by asnix_bshared_attrs_resolve's internal merge rounds
 * below. Returns the number of entries written (<= acount+bcount, shrinks
 * only on duplicate keys); `out_entries` must have room for at least
 * acount+bcount entries.
 *
 * M-garbage-fix perf note: deliberately NOT inlined. If the compiler
 * inlines this into the much-larger asnix_bshared_attrs_resolve below,
 * the function grows past whatever size threshold GCC uses to decide
 * whether a small-constant-size `memcpy(dst, src, ASNIX_ATTR_SIZE)` is
 * worth expanding into a couple of plain `mov`s inline -- past that
 * threshold it instead emits a real call to `__memcpy_chk` (confirmed
 * via `objdump -d`), which costs measurably more retired instructions
 * per entry than the handful of `mov`s asnix_attrs_eager_merge's own
 * (standalone, small, NOT inlined anywhere) copy of this same loop gets.
 * Keeping this helper out-of-line, same as asnix_attrs_eager_merge
 * itself, keeps it small enough that the compiler expands the memcpys
 * the same cheap way. */
static int64_t attrs_merge_entries_into(const char *aentries, int64_t acount, const char *bentries, int64_t bcount,
                                         char *out_entries) __attribute__((noinline));
static int64_t attrs_merge_entries_into(const char *aentries, int64_t acount, const char *bentries, int64_t bcount,
                                         char *out_entries) {
    const char *pa = aentries;
    const char *pb = bentries;
    const char *aend = aentries + acount * ASNIX_ATTR_SIZE;
    const char *bend = bentries + bcount * ASNIX_ATTR_SIZE;
    char *pout = out_entries;
    while (pa < aend && pb < bend) {
        uint32_t asym = *(const uint32_t *)(pa + ASNIX_ATTR_OFF_SYM);
        uint32_t bsym = *(const uint32_t *)(pb + ASNIX_ATTR_OFF_SYM);
        if (asym < bsym) {
            memcpy(pout, pa, ASNIX_ATTR_SIZE);
            pa += ASNIX_ATTR_SIZE;
        } else if (bsym < asym) {
            memcpy(pout, pb, ASNIX_ATTR_SIZE);
            pb += ASNIX_ATTR_SIZE;
        } else {
            /* duplicate key: rhs (b) wins, matching `//`'s right-biased semantics */
            memcpy(pout, pb, ASNIX_ATTR_SIZE);
            pa += ASNIX_ATTR_SIZE;
            pb += ASNIX_ATTR_SIZE;
        }
        pout += ASNIX_ATTR_SIZE;
    }
    while (pa < aend) {
        memcpy(pout, pa, ASNIX_ATTR_SIZE);
        pa += ASNIX_ATTR_SIZE;
        pout += ASNIX_ATTR_SIZE;
    }
    while (pb < bend) {
        memcpy(pout, pb, ASNIX_ATTR_SIZE);
        pb += ASNIX_ATTR_SIZE;
        pout += ASNIX_ATTR_SIZE;
    }
    return (pout - out_entries) / ASNIX_ATTR_SIZE;
}

/* M88: see builtins_shared.h's comment and heap.h's UnionHdr layout. */
Value asnix_bshared_attrs_resolve(Value v) {
    const char *base = (const char *)(uintptr_t)(v.raw & ~(uint64_t)ASNIX_TAG_MASK);
    int64_t count = *(const int64_t *)(base + ASNIX_ATTRS_OFF_COUNT);
    if (count >= 0) return v; /* already flat -- the common case */

    Value cached = *(const Value *)(base + ASNIX_ATTRS_UNION_OFF_FLATCACHE);
    if (cached.raw != 0) return cached; /* materialized by an earlier call */

    int64_t nleaves = -count;
    const Value *leaves = (const Value *)(base + ASNIX_ATTRS_UNION_OFF_LEAVES);

    if (nleaves == 1) {
        /* No merge at all needed -- leaves[0] is already a real,
         * permanent, flat attrs object (guaranteed flat by construction,
         * see the comment below), so there's nothing to copy. */
        Value acc = leaves[0];
        *(Value *)((char *)base + ASNIX_ATTRS_UNION_OFF_FLATCACHE) = acc;
        return acc;
    }

    /* Leaves are guaranteed flat by construction (asnix_attrs_update,
     * attrs_update.c, always resolves a would-be leaf before storing it).
     *
     * IMPORTANT: this is a bottom-up PAIRWISE (binary-tree) merge, NOT a
     * left-to-right fold (`acc = merge(acc, leaf)` repeated) -- a fold
     * would re-copy the ALREADY-MERGED, ever-growing accumulator on every
     * one of the (up to ASNIX_ATTRS_UNION_MAX_LEAVES-1) merge steps,
     * costing O(total_size * nleaves) instead of O(total_size *
     * log2(nleaves)). That was an actual bug found measuring this
     * milestone (see docs/architecture.md's M88 section): with the fold,
     * repeated materialization cost as much total copying as the eager
     * `//` path it was meant to avoid, measured via peak RSS on a
     * synthetic many-small-leaves benchmark (nearly identical memory to
     * the eager baseline). The pairwise merge below halves the work list
     * each round (same shape as mergesort's merge phase), preserving
     * `//`'s right-biased collision semantics: each pairwise merge of
     * (entries_ptr[i], entries_cnt[i]) with (entries_ptr[i+1],
     * entries_cnt[i+1]) keeps slot i+1 (the more-recently-layered,
     * higher-priority side, since `leaves` is stored lowest-priority-
     * first) as the winning operand on a duplicate key, and pairing
     * adjacent elements bottom-up preserves that same relative priority
     * order all the way up the tree.
     *
     * M-garbage-fix: the merge ALGORITHM above is unchanged from the
     * original bottom-up pairwise design -- same comparisons, same
     * copies, same round structure. What changed is WHERE each round's
     * output lives. Every round used to call asnix_attrs_eager_merge
     * directly, meaning every round's output -- including the (up to
     * nleaves-2) intermediate rounds that the very next round
     * immediately supersedes, and that nothing else ever reads again --
     * was a real, permanent asnix_arena_alloc allocation in the
     * never-free arena. Measured on the real ffmpeg-full.outPath
     * benchmark: materialize calls flatten 2,387,216 entries' worth of
     * actually-useful final results, but permanently allocated
     * 3,927,137 entries total across every round -- about 18.5MB of
     * pure, confirmed, never-reclaimed garbage.
     *
     * Fix: every round EXCEPT the last now writes into one of a handful
     * of plain `malloc` scratch buffers instead of the permanent arena.
     * NOT a ping-pong between just 2 buffers (a "carried over, unmerged"
     * odd-one-out entry from an earlier round can end up pointing into a
     * buffer a LATER round would then overwrite) -- one DEDICATED buffer
     * per round, never reused across rounds. ASNIX_ATTRS_UNION_MAX_LEAVES
     * (64) bounds the round count at log2(64)=6 (each round at least
     * halves the work list), so at most 6 scratch buffers are ever
     * allocated. Each is sized to `upper_bound` entries -- the total
     * entry count summed across all original leaves, which bounds every
     * round's output (a pairwise merge only ever shrinks the total, via
     * duplicate-key collisions, never grows it; the bound holds
     * transitively across rounds too).
     *
     * The LAST round is always a plain 2-way merge (the loop below only
     * runs while n > 2, and a round's output count is ceil(n/2), which
     * only reaches 1 by going through exactly n == 2) -- so instead of
     * writing it into one more scratch buffer and then copying the
     * result into the permanent arena (an extra full-size copy of the
     * final, biggest result, measured to cost ~19M extra instructions
     * on the real ffmpeg-full.outPath benchmark, enough on its own to
     * erase this fix's whole win), that last merge writes DIRECTLY into
     * one real, permanent arena allocation (upper-bound-sized, same
     * "don't bother reclaiming the slack" convention
     * asnix_attrs_eager_merge's own allocation already uses). Every
     * scratch buffer (if any were allocated at all) is freed right
     * after -- none of them outlive this function call. */
    const char *entries_ptr[ASNIX_ATTRS_UNION_MAX_LEAVES];
    int64_t entries_cnt[ASNIX_ATTRS_UNION_MAX_LEAVES];
    int64_t upper_bound = 0;
    for (int64_t i = 0; i < nleaves; i++) {
        const char *lb = (const char *)(uintptr_t)(leaves[i].raw & ~(uint64_t)ASNIX_TAG_MASK);
        int64_t lc = *(const int64_t *)(lb + ASNIX_ATTRS_OFF_COUNT);
        entries_ptr[i] = lb + ASNIX_ATTRS_OFF_ENTRIES;
        entries_cnt[i] = lc;
        upper_bound += lc;
    }

    void *scratch[6]; /* log2(ASNIX_ATTRS_UNION_MAX_LEAVES) == 6 */
    int64_t n_scratch = 0;
    size_t scratch_bytes = (size_t)upper_bound * ASNIX_ATTR_SIZE;
    if (scratch_bytes == 0) scratch_bytes = 1; /* malloc(0) is implementation-defined; avoid relying on it */

    int64_t n = nleaves;

    /* M-big-leaf-merge: the bottom-up pairwise tree below pays for a
     * disproportionately large leaf's own size at EVERY tree level (up to
     * log2(ASNIX_ATTRS_UNION_MAX_LEAVES)=6 times for a 64-leaf union), not
     * once. Real nixpkgs's classic `foldl' (acc: x: acc // x) {} bigList`
     * anti-pattern produces exactly this shape: leaves[0] is always the
     * ever-growing accumulator (asnix_attrs_update's make_union_header
     * always appends the NEW leaf after the EXISTING ones, so the oldest/
     * biggest leaf is always leftmost), while leaves[1..] are single-entry
     * leaves from each fold step. Ported from asnix's own M-big-leaf-merge
     * fix (builtins_shared.c), adapted to this file's scratch-buffer
     * design (no asnix_attrs_eager_merge call here at all, unlike asnix's
     * version -- see the M-garbage-fix comment above for why).
     *
     * Special case: when the biggest leaf alone outweighs every other leaf
     * combined, merge the small leaves together first (cheap, since
     * they're small) into one combined leaf using the exact same bottom-up
     * scratch-buffer round logic as the main loop below, then let the
     * UNCHANGED n==2 tail merge handle (leaf0, combined-small) as its
     * final, direct-to-arena merge -- O(big_size) paid once instead of
     * O(big_size * log2(nleaves)). Correctness: restricted to leaves[0]
     * specifically (not "whichever leaf happens to be biggest") because
     * that's the only position construction guarantees is safe --
     * attrs_merge_entries_into's right-biased collision rule (rhs always
     * wins) needs the big leaf on the lhs and everything newer-and-
     * combined on the rhs, which is only correct when the big leaf is the
     * OLDEST (lowest-priority) one, i.e. leaves[0]. The small-leaves-merge
     * below preserves their own relative priority order (newest-of-the-
     * rest wins) exactly as the unmodified main loop would. Setting `n =
     * 2` here and leaving entries_ptr[0]/entries_cnt[0] untouched makes
     * the `while (n > 2)` loop below a no-op, falling straight through to
     * the existing n==2 tail merge with no further change needed there. */
    if (nleaves >= 3) {
        int64_t rest_count = upper_bound - entries_cnt[0];
        if (entries_cnt[0] > rest_count) {
            const char *rptr[ASNIX_ATTRS_UNION_MAX_LEAVES];
            int64_t rcnt[ASNIX_ATTRS_UNION_MAX_LEAVES];
            for (int64_t i = 1; i < nleaves; i++) {
                rptr[i - 1] = entries_ptr[i];
                rcnt[i - 1] = entries_cnt[i];
            }
            int64_t rn = nleaves - 1;
            while (rn > 1) {
                void *mem = malloc(scratch_bytes);
                if (!mem) {
                    fprintf(stderr, "asnix: out of memory\n");
                    exit(1);
                }
                scratch[n_scratch++] = mem;
                char *cursor = (char *)mem;
                int64_t out_n = 0;
                int64_t i = 0;
                for (; i + 1 < rn; i += 2) {
                    int64_t k = attrs_merge_entries_into(rptr[i], rcnt[i], rptr[i + 1], rcnt[i + 1], cursor);
                    rptr[out_n] = cursor;
                    rcnt[out_n] = k;
                    out_n++;
                    cursor += k * ASNIX_ATTR_SIZE;
                }
                if (i < rn) {
                    rptr[out_n] = rptr[i];
                    rcnt[out_n] = rcnt[i];
                    out_n++;
                }
                rn = out_n;
            }
            entries_ptr[1] = rptr[0];
            entries_cnt[1] = rcnt[0];
            n = 2;
        }
    }

    while (n > 2) {
        void *mem = malloc(scratch_bytes);
        if (!mem) {
            fprintf(stderr, "asnix: out of memory\n");
            exit(1);
        }
        scratch[n_scratch++] = mem;
        char *cursor = (char *)mem;

        int64_t out_n = 0;
        int64_t i = 0;
        for (; i + 1 < n; i += 2) {
            int64_t k = attrs_merge_entries_into(entries_ptr[i], entries_cnt[i], entries_ptr[i + 1],
                                                  entries_cnt[i + 1], cursor);
            entries_ptr[out_n] = cursor;
            entries_cnt[out_n] = k;
            out_n++;
            cursor += k * ASNIX_ATTR_SIZE;
        }
        if (i < n) { /* odd one out, carried up unmerged */
            entries_ptr[out_n] = entries_ptr[i];
            entries_cnt[out_n] = entries_cnt[i];
            out_n++;
        }
        n = out_n;
    }

    /* n == 2 here (see the comment above): the one and only real,
     * permanent allocation this function ever makes, built directly
     * from the last pairwise merge's own output -- no extra copy. */
    void *mem = asnix_arena_alloc(ASNIX_ATTRS_OFF_ENTRIES + (uint64_t)(entries_cnt[0] + entries_cnt[1]) * ASNIX_ATTR_SIZE);
    int64_t final_count = attrs_merge_entries_into(entries_ptr[0], entries_cnt[0], entries_ptr[1], entries_cnt[1],
                                                     (char *)mem + ASNIX_ATTRS_OFF_ENTRIES);
    *(int64_t *)((char *)mem + ASNIX_ATTRS_OFF_COUNT) = final_count;
    asnix_stats_attrs++;
    asnix_stats_attrs_entries += (uint64_t)final_count;

    for (int64_t s = 0; s < n_scratch; s++) free(scratch[s]);

    Value acc = (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_ATTRS };
    *(Value *)((char *)base + ASNIX_ATTRS_UNION_OFF_FLATCACHE) = acc;
    return acc;
}

int64_t asnix_bshared_attrs_count(Value v) {
    v = asnix_bshared_attrs_resolve(v);
    return *(const int64_t *)((const char *)(uintptr_t)(v.raw & ~(uint64_t)ASNIX_TAG_MASK) + ASNIX_ATTRS_OFF_COUNT);
}

uint32_t asnix_bshared_attrs_sym(Value v, int64_t i) {
    v = asnix_bshared_attrs_resolve(v);
    const char *entry =
        (const char *)(uintptr_t)(v.raw & ~(uint64_t)ASNIX_TAG_MASK) + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
    return *(const uint32_t *)(entry + ASNIX_ATTR_OFF_SYM);
}

Value asnix_bshared_attrs_value(Value v, int64_t i) {
    v = asnix_bshared_attrs_resolve(v);
    const char *entry =
        (const char *)(uintptr_t)(v.raw & ~(uint64_t)ASNIX_TAG_MASK) + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
    return *(const Value *)(entry + ASNIX_ATTR_OFF_VALUE);
}

/* fork (unionhdr-probe): search on a value ALREADY KNOWN to be flat
 * (count >= 0) -- shared by asnix_bshared_attrs_probe and (M114)
 * asnix_bshared_attrs_find below, raw-layout version without any
 * resolve call, since every leaf of a union is already guaranteed flat
 * by construction (asnix_attrs_update always resolves a would-be leaf
 * before storing it), and a caller that has already resolved its own
 * `attrs` argument once knows the result is flat too.
 *
 * M-attrs-linear-scan: iets's own get_flat (runtime/value.rs) uses a
 * LINEAR scan, not binary search, whenever the set has 8 or fewer
 * entries -- read directly, not guessed: "binary search over symbol
 * ids; linear for small sets". A sequential scan over a small, tightly-
 * packed entry array beats binary search's O(log n) but branch-heavy,
 * cache-unpredictable jump pattern for small n, and most real nixpkgs
 * attrsets (package metadata, module options) are small. Kept the
 * exact threshold (8) iets itself uses. */
static int64_t attrs_find_flat_noresolve(Value flat, uint32_t sym) {
    const char *base = (const char *)(uintptr_t)(flat.raw & ~(uint64_t)ASNIX_TAG_MASK);
    int64_t hi = *(const int64_t *)(base + ASNIX_ATTRS_OFF_COUNT);
    const char *entries = base + ASNIX_ATTRS_OFF_ENTRIES;
    if (hi <= 8) {
        const char *entry = entries;
        for (int64_t i = 0; i < hi; i++, entry += ASNIX_ATTR_SIZE) {
            if (*(const uint32_t *)(entry + ASNIX_ATTR_OFF_SYM) == sym) return i;
        }
        return -1;
    }
    int64_t lo = 0;
    while (lo < hi) {
        int64_t mid = lo + (hi - lo) / 2;
        const char *entry = entries + mid * ASNIX_ATTR_SIZE;
        uint32_t s = *(const uint32_t *)(entry + ASNIX_ATTR_OFF_SYM);
        if (s == sym) return mid;
        if (s < sym) lo = mid + 1; else hi = mid;
    }
    return -1;
}

/* M114 perf: was `resolve(attrs)` once + `attrs_count(attrs)` (another
 * internal resolve) to get `hi`, then `attrs_sym(attrs, mid)` per binary-
 * search iteration (which ALSO internally resolves `attrs` on every
 * call, even though `attrs` is loop-invariant and was already resolved
 * on the line above). For a typical nixpkgs-sized attrset that's
 * 1 (direct) + 1 (via count) + ~log2(n) (via sym) resolve() calls per
 * find(), where exactly one suffices: `attrs` is already flat (count >=
 * 0) immediately after the one resolve() call below, so the rest of the
 * search can walk the raw entry array directly via
 * attrs_find_flat_noresolve, exactly like M93's attrs_probe/
 * attrs_find_flat_noresolve pairing already does for the op_select.S
 * fast path (see that function's comment -- this was its flagged,
 * explicitly-out-of-scope-at-the-time follow-up for attrs_find/_count/
 * _sym/_value). */
int64_t asnix_bshared_attrs_find(Value attrs, uint32_t sym) {
    attrs = asnix_bshared_attrs_resolve(attrs);
    return attrs_find_flat_noresolve(attrs, sym);
}

/* fork (unionhdr-probe): bounded multi-leaf probe for a SINGLE-KEY
 * lookup on a lazy union (heap.h's UnionHdr, M88/M93). The existing
 * asnix_bshared_attrs_find/_resolve ALWAYS fully materializes+caches
 * every leaf merged together (a pairwise merge across all leaves, same
 * cost as an eager `//`) before doing ANY lookup -- even when the caller
 * only wants ONE key, which is op_select.S's `a.b` access, the single
 * most common real-world attrs operation in compiled Nix code. That
 * means M88's lazy deferral provides essentially zero benefit the
 * moment an attrset is touched even once via a point lookup, which is
 * nearly always, since why build `a // b` and never read from it --
 * found by tracing exactly where every access path routes through
 * asnix_bshared_attrs_resolve unconditionally.
 *
 * Real Nix's own layered Bindings::get() never has this problem: a
 * point lookup walks the layer chain directly (binary-search the top
 * layer, fall through to baseLayer on miss), with no flattened view
 * ever required unless something genuinely needs full iteration. This
 * function is the same idea for asnix's leaf-array union representation
 * -- walk leaves from HIGHEST priority (last-stored, matching `//`'s
 * right-biased override semantics -- heap.h's own comment confirms
 * `leaves` is stored lowest-priority-first) to lowest, binary-search
 * each flat leaf directly, return on first hit. Never touches
 * flat_cache, never allocates, never mutates the union. If the union
 * was ALREADY materialized by some earlier full-iteration caller, probe
 * that cached flat form directly instead (still just one binary search,
 * no leaf-walk needed).
 *
 * Deliberately narrow in scope: only wired into op_select.S's single-key
 * `a.b` path (the hottest, most frequent case) for this one-shot fork,
 * NOT into asnix_bshared_attrs_find/_count/_sym/_value, whose 35+
 * callers expect a stable POSITIONAL index into v's own flat form --
 * changing that contract to avoid materialization too would need
 * auditing every one of those call sites, out of scope here. */
int asnix_bshared_attrs_probe(Value attrs, uint32_t sym, Value *out_value) {
    const char *base = (const char *)(uintptr_t)(attrs.raw & ~(uint64_t)ASNIX_TAG_MASK);
    int64_t count = *(const int64_t *)(base + ASNIX_ATTRS_OFF_COUNT);
    Value probe_target = attrs;
    if (count < 0) {
        Value cached = *(const Value *)(base + ASNIX_ATTRS_UNION_OFF_FLATCACHE);
        if (cached.raw != 0) {
            probe_target = cached;
        } else {
            int64_t nleaves = -count;
            const Value *leaves = (const Value *)(base + ASNIX_ATTRS_UNION_OFF_LEAVES);
            for (int64_t i = nleaves - 1; i >= 0; i--) {
                int64_t idx = attrs_find_flat_noresolve(leaves[i], sym);
                if (idx < 0) continue;
                const char *lbase = (const char *)(uintptr_t)(leaves[i].raw & ~(uint64_t)ASNIX_TAG_MASK);
                const char *entry = lbase + ASNIX_ATTRS_OFF_ENTRIES + idx * ASNIX_ATTR_SIZE;
                *out_value = *(const Value *)(entry + ASNIX_ATTR_OFF_VALUE);
                return 1;
            }
            return 0;
        }
    }
    int64_t idx = attrs_find_flat_noresolve(probe_target, sym);
    if (idx < 0) return 0;
    const char *pbase = (const char *)(uintptr_t)(probe_target.raw & ~(uint64_t)ASNIX_TAG_MASK);
    const char *entry = pbase + ASNIX_ATTRS_OFF_ENTRIES + idx * ASNIX_ATTR_SIZE;
    *out_value = *(const Value *)(entry + ASNIX_ATTR_OFF_VALUE);
    return 1;
}

typedef struct {
    uint32_t sym;
    int64_t idx;
} MakeAttrsEntry;

/* Threshold below which plain insertion sort beats a merge sort's loop/
 * merge overhead, plus any scratch allocation at all -- the exact same
 * tradeoff, and the same constant, as compiler.c's AST_ATTRSET case uses
 * for its own compile-time sort (see that case's
 * ASNIX_ATTRSET_SORT_INSERTION_MAX / ASNIX_COMPILE_SCRATCH_MAX comments;
 * not shared via a header since the two sorts operate on unrelated data
 * and this file has no other reason to depend on compiler.c). M158:
 * instrumentation on a full nixpkgs eval (`./asnix d.nix`) showed 93% of
 * all asnix_bshared_make_attrs calls have count <= 32. */
#define ASNIX_MAKE_ATTRS_SORT_INSERTION_MAX 32

/* perf: bottom-up (iterative) merge sort over `.sym` alone, specialized
 * for `MakeAttrsEntry` -- replaces a `qsort`+indirect-comparator call for
 * the above-threshold path. Same O(n log n) algorithm `qsort_r` already
 * ran internally (glibc's own `msort_with_tmp`); the win here is not
 * algorithmic, it's removing the two things a type-erased C library sort
 * cannot avoid: an indirect call to `cmp_make_attrs_entry` for every
 * single comparison, and generic byte-range (`memmove`-style) element
 * moves instead of a direct 16-byte struct copy. `iets`'s equivalent
 * (`sort_by_key`/`Vec::sort`, src/runtime/value.rs) pays neither cost --
 * Rust monomorphizes and inlines the key-extraction closure directly into
 * the sort, with no indirection -- so this closes exactly that gap, not a
 * new algorithmic idea. Naturally stable (ties always resolved in favor
 * of the left run, i.e. the earlier original index) by construction, so
 * it reproduces the old `cmp_make_attrs_entry`'s explicit `(sym, idx)`
 * tie-break exactly without ever comparing `.idx` at all -- a duplicate
 * sym's earlier occurrence always stays first, same as insertion sort's
 * `>` (never swaps equal syms) and the same as `iets`'s own `sort_by_key`
 * (also stable). `tmp` must be a second buffer of the same size as `a`;
 * returns whichever of `a`/`tmp` ends up holding the fully-sorted result
 * (alternates each pass, so for odd counts of passes it is `tmp`). */
static MakeAttrsEntry *merge_sort_make_attrs_entries(MakeAttrsEntry *a, MakeAttrsEntry *tmp, int64_t n) {
    MakeAttrsEntry *src = a, *dst = tmp;
    for (int64_t width = 1; width < n; width *= 2) {
        for (int64_t i = 0; i < n; i += 2 * width) {
            int64_t mid = i + width < n ? i + width : n;
            int64_t end = i + 2 * width < n ? i + 2 * width : n;
            int64_t l = i, r = mid, k = i;
            while (l < mid && r < end) {
                if (src[l].sym <= src[r].sym) dst[k++] = src[l++];
                else dst[k++] = src[r++];
            }
            while (l < mid) dst[k++] = src[l++];
            while (r < end) dst[k++] = src[r++];
        }
        MakeAttrsEntry *t = src; src = dst; dst = t;
    }
    return src;
}

/* M89 perf: was an insertion sort (O(n^2) worst case) -- measured on a
 * full nixpkgs evaluation (`./asnix d.nix`, ffmpeg.outPath): 52,539
 * calls, up to 5,234 entries in a single call, ~699,000 total swaps.
 * Switched to qsort over (sym, original-index) pairs, same "resolve
 * once into a sortable key, sort that" approach M88 already used for
 * `sorted_name_order` -- see docs/architecture.md's M88/M89 notes.
 *
 * M158 perf: that switch made EVERY call pay qsort's malloc/free pair
 * and per-comparison indirect call through cmp_make_attrs_entry, even
 * though 93% of real-workload calls have count <= 32 (measured above).
 * Below that threshold, sort a plain index array in place with
 * insertion sort directly on the stack -- no allocation, no indirect
 * comparator call. Insertion sort is stable by construction (the
 * `>` comparison below never swaps equal syms), so it preserves the
 * same "ties keep original relative order" behavior the old insertion
 * sort and cmp_make_attrs_entry's explicit idx tie-break both gave,
 * without needing to track idx as a separate field at all.
 *
 * perf (this milestone): a fresh profile found the ABOVE-threshold path
 * still real: 3,662 calls on the real d.nix/ffmpeg eval (7% of all
 * calls, M161 left this branch unchanged), 171,246 total entries sorted
 * (avg ~47/call, max 5,234), `msort_with_tmp`/`qsort_r` self-time ~0.8%
 * of the whole eval's instructions. Replaced `malloc`+`qsort`+`free`
 * with two `asnix_arena_alloc` scratch buffers (never individually
 * freed -- same "leak it into the never-free bump arena, it's cheap and
 * short-lived" tradeoff M171 already made for IrBuilder's `ops`/`pool`
 * arrays; ~47 entries * 16 bytes * 2 buffers * 3,662 calls ~= 5.5 MB
 * total over the whole eval, trivial against this process's baseline
 * RSS) plus the hand-written `merge_sort_make_attrs_entries` above. */
Value asnix_bshared_make_attrs(uint32_t *syms, Value *vals, int64_t count) {
    void *mem = asnix_arena_alloc(8 + (uint64_t)count * ASNIX_ATTR_SIZE);
    asnix_stats_attrs++;
    asnix_stats_attrs_entries += (uint64_t)count;
    *(int64_t *)((char *)mem + ASNIX_ATTRS_OFF_COUNT) = count;

    if (count <= ASNIX_MAKE_ATTRS_SORT_INSERTION_MAX) {
        int64_t order_stack[ASNIX_MAKE_ATTRS_SORT_INSERTION_MAX];
        for (int64_t i = 0; i < count; i++) order_stack[i] = i;
        for (int64_t i = 1; i < count; i++) {
            int64_t j = i;
            while (j > 0 && syms[order_stack[j - 1]] > syms[order_stack[j]]) {
                int64_t tmp = order_stack[j - 1];
                order_stack[j - 1] = order_stack[j];
                order_stack[j] = tmp;
                j--;
            }
        }
        for (int64_t i = 0; i < count; i++) {
            char *entry = (char *)mem + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
            int64_t src = order_stack[i];
            /* M103: must be a 4-byte write -- entry's value bytes now start right after sym (no pad),
             * so an 8-byte write here would clobber them. */
            *(uint32_t *)(entry + ASNIX_ATTR_OFF_SYM) = syms[src];
            *(Value *)(entry + ASNIX_ATTR_OFF_VALUE) = vals[src];
        }
    } else {
        MakeAttrsEntry *entries = asnix_arena_alloc((uint64_t)count * sizeof(MakeAttrsEntry));
        MakeAttrsEntry *scratch = asnix_arena_alloc((uint64_t)count * sizeof(MakeAttrsEntry));
        for (int64_t i = 0; i < count; i++) {
            entries[i].sym = syms[i];
            entries[i].idx = i;
        }
        MakeAttrsEntry *sorted = merge_sort_make_attrs_entries(entries, scratch, count);
        for (int64_t i = 0; i < count; i++) {
            char *entry = (char *)mem + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
            int64_t src = sorted[i].idx;
            *(uint32_t *)(entry + ASNIX_ATTR_OFF_SYM) = sorted[i].sym;
            *(Value *)(entry + ASNIX_ATTR_OFF_VALUE) = vals[src];
        }
    }
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_ATTRS };
}

/* perf: for every one of `__overrides`'s `ocount` entries, this used to
 * linearly scan all `n` of the enclosing `rec {}` block's static bindings
 * (`meta[0..n-1]`) looking for a same-sym match -- O(n*ocount) overall,
 * no size threshold at all (same anti-pattern class as this session's
 * other dedup/membership fixes, e.g. prim_removeAttrs_apply2 just above
 * in builtins_attrs.c). `rec {}`'s own static bindings are necessarily
 * distinct syms (a literal attrset/rec block can't declare the same name
 * twice), so sorting a (sym, original Env-slot index) side array once and
 * binary-searching it per override -- rather than reaching for an open-
 * addressing hash table like string_context.c's fix needed -- is enough:
 * this mechanism is rarely used in real Nix (`rec { __overrides = {...};
 * ... }`) and `n` is bounded by how many bindings a single `rec {}` block
 * has (realistically small), so the win is mostly about not regressing
 * badly on a pathological case rather than a measured hot path. Same
 * insertion-sort-below/qsort-above threshold convention as the rest of
 * this codebase (make_attrs's ASNIX_MAKE_ATTRS_SORT_INSERTION_MAX). */
#define ASNIX_REC_OVERRIDES_SORT_INSERTION_MAX 32

/* perf (follow-up): building+sorting `order` UNCONDITIONALLY measured as a
 * real, reproducible regression on this project's own ffmpeg-full/plasma-
 * system benchmarks (callgrind) -- real `rec{}` blocks with `__overrides`
 * overwhelmingly have few static bindings, where the malloc+sort setup
 * costs more than the linear scan it replaces. Gated on the PRODUCT
 * `n * ocount` (same calibration lesson as builtins_attrs.c's removeAttrs
 * fix, ASNIX_REMOVE_ATTRS_PRODUCT_MAX -- gating on `n` alone still pays
 * sort overhead for a "few static bindings, many override entries" shape
 * where the OLD per-override linear scan was already cheap because `n`
 * itself was small) -- below that, skip the index and fall back to the
 * original O(n * ocount) linear-scan-per-override behavior (byte-for-byte
 * the old cost); only once the product crosses it does the sorted-index
 * path actually pay for itself. */
#define ASNIX_REC_OVERRIDES_PRODUCT_MAX 2048

typedef struct { uint32_t sym; int64_t idx; } RecOverridesEntry;

static int cmp_rec_overrides_entry(const void *a, const void *b) {
    const RecOverridesEntry *ea = a, *eb = b;
    if (ea->sym != eb->sym) return ea->sym < eb->sym ? -1 : 1;
    return 0;
}

static int64_t rec_overrides_find(const RecOverridesEntry *sorted, int64_t n, uint32_t sym) {
    int64_t lo = 0, hi = n - 1;
    while (lo <= hi) {
        int64_t mid = lo + (hi - lo) / 2;
        if (sorted[mid].sym == sym) return sorted[mid].idx;
        if (sorted[mid].sym < sym) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

Value asnix_bshared_rec_overrides_merge(Env *env, int64_t n, const int64_t *meta) {
    int64_t overrides_slot = meta[n];
    Value overrides_val = asnix_force_for_print(env->values[overrides_slot]);
    if (asnix_tag_of(overrides_val) != ASNIX_TAG_ATTRS) {
        fprintf(stderr, "asnix: '__overrides' must be a set (got %s)\n",
                asnix_bshared_type_name(overrides_val));
        exit(1);
    }

    /* Working (sym, value) pairs: the n static bindings first (by Env
     * slot order, matched ones replaced below), then any override-only
     * extras appended -- asnix_bshared_make_attrs sorts the whole lot by
     * sym at the end, so this function never needs to track order itself. */
    int64_t ocount = asnix_bshared_attrs_count(overrides_val);
    int64_t cap = n + ocount;
    uint32_t *syms = malloc((size_t)cap * sizeof(uint32_t));
    Value *vals = malloc((size_t)cap * sizeof(Value));
    for (int64_t i = 0; i < n; i++) {
        syms[i] = (uint32_t)meta[i];
        vals[i] = env->values[i];
    }

    /* Gated on the PRODUCT (see ASNIX_REC_OVERRIDES_PRODUCT_MAX's own
     * comment above) -- below that, `order` is never built and the loop
     * below falls back to the original linear scan untouched. */
    int use_sorted = n * ocount > ASNIX_REC_OVERRIDES_PRODUCT_MAX;
    RecOverridesEntry *order = NULL;
    if (use_sorted) {
        order = malloc((size_t)n * sizeof(RecOverridesEntry));
        for (int64_t i = 0; i < n; i++) order[i] = (RecOverridesEntry){ .sym = syms[i], .idx = i };
        if (n <= ASNIX_REC_OVERRIDES_SORT_INSERTION_MAX) {
            for (int64_t i = 1; i < n; i++) {
                RecOverridesEntry e = order[i];
                int64_t j = i;
                while (j > 0 && order[j - 1].sym > e.sym) {
                    order[j] = order[j - 1];
                    j--;
                }
                order[j] = e;
            }
        } else {
            qsort(order, (size_t)n, sizeof(RecOverridesEntry), cmp_rec_overrides_entry);
        }
    }

    int64_t total = n;
    for (int64_t oi = 0; oi < ocount; oi++) {
        uint32_t osym = asnix_bshared_attrs_sym(overrides_val, oi);
        Value oval = asnix_bshared_attrs_value(overrides_val, oi);
        int64_t match;
        if (use_sorted) {
            match = rec_overrides_find(order, n, osym);
        } else {
            match = -1;
            for (int64_t i = 0; i < n; i++) {
                if (syms[i] == osym) { match = i; break; }
            }
        }
        if (match >= 0) {
            vals[match] = oval;
            env->values[match] = oval; /* propagate into sibling self-references */
        } else {
            syms[total] = osym;
            vals[total] = oval;
            total++;
        }
    }
    free(order);
    Value out = asnix_bshared_make_attrs(syms, vals, total);
    free(syms);
    free(vals);
    return out;
}

void asnix_bshared_string_bytes(Value v, const char **bytes_out, int64_t *len_out, const char *who) {
    if (asnix_tag_of(v) != ASNIX_TAG_PTR) {
        fprintf(stderr, "asnix: %s: expected a string argument\n", who);
        exit(1);
    }
    *bytes_out = asnix_ptr_bytes(v, len_out);
}

/* M67: real Nix's `eqValues` (eval.cc) special-cases attrsets that denote
 * a derivation -- `isDerivation(v)` checks for a `type` attribute whose
 * forced value is exactly the string "derivation" -- comparing them by
 * their `outPath` attribute's value (itself a recursive `eqValues` call,
 * not a raw byte-compare) rather than the generic attribute-by-attribute
 * structural walk below. Verified directly against real `nix-instantiate
 * --eval`: two separately-constructed `derivation {...}` calls with
 * identical args are `==` (same outPath) even though they're two
 * different heap attrsets; `derivation {...} // { dummy = 1; }` is still
 * `==` to the original (outPath is unaffected by an unrelated `//`
 * merge); two derivations differing only in `builder` are not `==`
 * (different outPath). Needed because the generic structural walk below
 * would otherwise (a) give the wrong answer whenever a `//` merge changes
 * the attribute count/shape without changing outPath, and (b) risk
 * forcing attributes real Nix never would (e.g. a `throw`-valued
 * attribute merged on afterward) just to decide structural equality. */
static int value_is_derivation(Value v) {
    if (asnix_tag_of(v) != ASNIX_TAG_ATTRS) return 0;
    uint32_t sym_type = asnix_intern("type", 4);
    int64_t idx = asnix_bshared_attrs_find(v, sym_type);
    if (idx < 0) return 0;
    Value tv = asnix_force_for_print(asnix_bshared_attrs_value(v, idx));
    if (asnix_tag_of(tv) != ASNIX_TAG_PTR || asnix_value_is_path(tv) || asnix_value_is_float(tv)) return 0;
    int64_t len;
    const char *bytes = asnix_ptr_bytes(tv, &len);
    return len == 10 && memcmp(bytes, "derivation", 10) == 0;
}

/* M92: real Nix's `eqValues` (eval.cc ~line 2919) starts with `if (&v1 ==
 * &v2) return true` -- comparing the ADDRESS of the Value slot/cell
 * itself, BEFORE forcing either side, not the heap payload either one
 * eventually forces to. asnix has no single uniform "every Value lives in
 * an addressable cell" model the way real Nix does (see heap.h's
 * ASNIX_CELL_MARKER comment) -- the closest equivalents are (a) two Cells
 * (possibly different Cell objects, from two independent OP_VAR_REF_CELL
 * executions) whose `target` fields point at the exact same Env slot, and
 * (b) two Values that are the literal same raw THUNK -- genuinely shared
 * memoization storage, e.g. the same already-forced attrset's entry read
 * twice (`builtins.attrValues x == builtins.attrValues x`). Must run on
 * the RAW, pre-force operands -- forcing loses exactly the identity
 * information being checked (confirmed via numtide/nix-conformance's
 * eval-okay-eq-slot-elem.nix/-identity.nix).
 *
 * Deliberately does NOT do a blanket `a.raw == b.raw` on any already-
 * concrete tag (INT/IMM/PTR/LIST/ATTRS/CLOSURE/PRIMOP): `==`'s own two
 * direct operands (asnix_bshared_value_eq_op) are ALWAYS already fully
 * forced by the ordinary (always-forcing) OP_VAR_REF/OP_SELECT/etc.
 * chokepoint that produced them -- a bare variable used directly as an
 * operand is never Cell-ified (compiler.c's try_cell_ref only fires at
 * `ib_mk_thunk` call sites, never for an ordinary compile_expr(AST_VAR)
 * read) -- so two closures that happen to be bit-identical CONTENT
 * (e.g. `let f = x: x; in f == f`, both reads forced down to the same
 * underlying closure object) are NOT "the same slot" in real Nix's
 * sense (ExprOpEq::eval always copies into a fresh local Value first --
 * see builtins_shared.h's own M80 comment) and must still compare
 * `false` (eval-okay-equal-function-direct-identical.nix) -- exactly
 * the case a blanket raw-equality check would have wrongly short-
 * circuited to `true`. List/Attrs pointer-equality is still handled,
 * correctly, by asnix_bshared_value_eq's own LIST/ATTRS cases below
 * (content equality there IS genuine equality, unlike for a function). */
static int asnix_bshared_slot_identical(Value a, Value b) {
    if (asnix_value_is_cell(a) && asnix_value_is_cell(b)) return asnix_cell_target(a) == asnix_cell_target(b);
    if (asnix_tag_of(a) == ASNIX_TAG_THUNK && asnix_tag_of(b) == ASNIX_TAG_THUNK) return a.raw == b.raw;
    return 0;
}

int asnix_bshared_value_eq(Value a, Value b) {
    if (asnix_bshared_slot_identical(a, b)) return 1;
    a = asnix_force_for_print(a);
    b = asnix_force_for_print(b);
    int ta = asnix_tag_of(a), tb = asnix_tag_of(b);
    /* M51: a Float on either side -- checked FIRST, before the generic
     * PTR-vs-PTR string/path byte-compare below (a Float's bytes are a
     * marker+double, not length-prefixed text, so that branch would
     * misread them) and before the plain `ta != tb` type-mismatch
     * rejection (int-vs-float must still compare equal when numerically
     * equal, matching real Nix's own `==`/eqValues -- confirmed against
     * `nix-instantiate --eval`: `1 == 1.0` is `true`). Not numeric on
     * one side and not float-tagged on the other (e.g. a Float vs a
     * string) simply isn't equal, same as any other cross-type `==`. */
    if (asnix_value_is_float(a) || asnix_value_is_float(b)) {
        double da, db;
        if (asnix_value_is_float(a)) da = asnix_float_value(a);
        else if (ta == ASNIX_TAG_INT) da = (double)asnix_int_value(a);
        else return 0;
        if (asnix_value_is_float(b)) db = asnix_float_value(b);
        else if (tb == ASNIX_TAG_INT) db = (double)asnix_int_value(b);
        else return 0;
        return da == db;
    }
    if (ta == ASNIX_TAG_PTR && tb == ASNIX_TAG_PTR) {
        int64_t la, lb;
        const char *ba = asnix_ptr_bytes(a, &la);
        const char *bb = asnix_ptr_bytes(b, &lb);
        return la == lb && (la == 0 || memcmp(ba, bb, (size_t)la) == 0);
    }
    if (ta != tb) return 0;
    switch (ta) {
        case ASNIX_TAG_INT: return asnix_int_value(a) == asnix_int_value(b);
        case ASNIX_TAG_IMM: return asnix_imm_subtag(a) == asnix_imm_subtag(b);
        case ASNIX_TAG_LIST: {
            if (a.raw == b.raw) return 1;
            int64_t na = asnix_bshared_list_count(a), nb = asnix_bshared_list_count(b);
            if (na != nb) return 0;
            const Value *ea = asnix_bshared_list_elems(a), *eb = asnix_bshared_list_elems(b);
            for (int64_t i = 0; i < na; i++) {
                if (!asnix_bshared_value_eq(ea[i], eb[i])) return 0;
            }
            return 1;
        }
        case ASNIX_TAG_ATTRS: {
            if (a.raw == b.raw) return 1;
            if (value_is_derivation(a) && value_is_derivation(b)) {
                uint32_t sym_outpath = asnix_intern("outPath", 7);
                int64_t ia = asnix_bshared_attrs_find(a, sym_outpath);
                int64_t ib = asnix_bshared_attrs_find(b, sym_outpath);
                if (ia >= 0 && ib >= 0) {
                    return asnix_bshared_value_eq(asnix_bshared_attrs_value(a, ia), asnix_bshared_attrs_value(b, ib));
                }
            }
            int64_t na = asnix_bshared_attrs_count(a), nb = asnix_bshared_attrs_count(b);
            if (na != nb) return 0;
            for (int64_t i = 0; i < na; i++) {
                if (asnix_bshared_attrs_sym(a, i) != asnix_bshared_attrs_sym(b, i)) return 0;
                if (!asnix_bshared_value_eq(asnix_bshared_attrs_value(a, i), asnix_bshared_attrs_value(b, i)))
                    return 0;
            }
            return 1;
        }
        /* M92: closures/primops (the only tags that can still reach here --
         * INT/IMM/PTR/LIST/ATTRS are all handled above, and THUNK is
         * impossible: force_for_print already fully resolved both sides)
         * are NEVER equal at this point, matching real Nix's `case
         * nFunction: return false` (eval.cc), which runs unconditionally
         * once the `&v1==&v2` slot-identity shortcut hasn't already
         * fired -- and it HAS already been checked, above, on the raw
         * pre-force operands (asnix_bshared_slot_identical). Previously
         * this fell back to `a.raw == b.raw` (forced-object pointer
         * identity), which happened to coincide with real Nix's answer
         * whenever the SAME Thunk/Cell happened to be read from two
         * places (there was no other way for asnix to express "same
         * slot") but gave the WRONG answer (true) whenever two
         * INDEPENDENTLY-computed closures just happened to evaluate to
         * the literal same heap object -- e.g. `[(id id)] == [(id id)]`:
         * each `(id id)` is a fresh, unaliased Thunk that both happen to
         * evaluate to the very same `id` closure; real Nix says `false`
         * (confirmed via numtide/nix-conformance's
         * eval-okay-eq-slot-elem.nix/-identity.nix, which this fixes).
         * `asnix_bshared_slot_identical` above is what keeps the cases
         * that DO need `true` (eval-okay-eq-slot-attrs.nix's `g = x: x;`
         * field, reached twice through the literal same shared Thunk; a
         * bare-variable aliased pair via a Cell) working. */
        default: return 0;
    }
}

/* See builtins_shared.h's long M80 comment. M92: also checks the Cell
 * slot-identity shortcut (asnix_bshared_slot_identical) on the RAW,
 * PRE-FORCE `a`/`b` -- before EITHER of this function's own two
 * `force_for_print` calls, which would otherwise discard exactly the
 * aliasing information being checked. In practice `==`'s own two direct
 * operands are compiled via the ordinary (always-forcing) OP_VAR_REF/
 * OP_SELECT/etc. chokepoints, never OP_VAR_REF_CELL (that's only ever
 * emitted at the ten `ib_mk_thunk` call sites, see compiler.c's
 * `try_cell_ref`) -- so a raw Cell basically never reaches here directly
 * -- but checking costs one cheap tag test and keeps this function
 * exactly as correct as `asnix_bshared_value_eq`'s own recursive check,
 * with no special-case asymmetry between the two. */
int asnix_bshared_value_eq_op(Value a, Value b) {
    if (asnix_bshared_slot_identical(a, b)) return 1;
    Value fa = asnix_force_for_print(a);
    Value fb = asnix_force_for_print(b);
    int ta = asnix_tag_of(fa), tb = asnix_tag_of(fb);
    if (ta == tb && (ta == ASNIX_TAG_CLOSURE || ta == ASNIX_TAG_PRIMOP)) return 0;
    return asnix_bshared_value_eq(fa, fb);
}

Value asnix_bshared_select_dynamic(Value target, Value name_val) {
    if (asnix_tag_of(target) != ASNIX_TAG_ATTRS) {
        fprintf(stderr, "asnix: cannot select an attribute (dynamic) on a non-attrset value (got %s)\n",
                asnix_bshared_type_name(target));
        exit(1);
    }
    if (asnix_tag_of(name_val) != ASNIX_TAG_PTR || asnix_value_is_path(name_val) || asnix_value_is_float(name_val)) {
        fprintf(stderr, "asnix: a dynamic '.${...}' attribute name must be a string (got %s)\n",
                asnix_bshared_type_name(name_val));
        exit(1);
    }
    int64_t len;
    const char *bytes = asnix_ptr_bytes(name_val, &len);
    uint32_t sym = asnix_intern(bytes, (size_t)len);
    int64_t idx = asnix_bshared_attrs_find(target, sym);
    if (idx < 0) {
        fprintf(stderr, "asnix: attribute '%.*s' missing, aborting\n", (int)len, bytes);
        exit(1);
    }
    return asnix_force_for_print(asnix_bshared_attrs_value(target, idx));
}

/* `expr.${nameExpr} or default` (M28) -- OP_SELECT_DYNAMIC_OR's C half.
 * Unlike asnix_bshared_select_dynamic above (fatal on a non-attrset
 * target or a missing key), a non-attrset target here falls through
 * gracefully to the default too (same "not even an attrset simply means
 * not found" rule OP_SELECT_OR/OP_HAS_ATTR already established for `or`/
 * `?`) -- only the name itself being a non-string is still a hard fatal
 * (that's a genuine misuse, not a "not found"). Returns 1 and writes the
 * forced value through `*out` on a hit, 0 (leaving `*out` untouched) on
 * a miss -- the caller (op_select_dynamic_or.S) decides whether to push
 * the value or jump to the default's code, same split OP_SELECT_OR's own
 * inline asm makes for the static case. */
int asnix_bshared_select_dynamic_or(Value target, Value name_val, Value *out) {
    if (asnix_tag_of(target) != ASNIX_TAG_ATTRS) return 0;
    if (asnix_tag_of(name_val) != ASNIX_TAG_PTR || asnix_value_is_path(name_val) || asnix_value_is_float(name_val)) {
        fprintf(stderr, "asnix: a dynamic '.${...}' attribute name must be a string (got %s)\n",
                asnix_bshared_type_name(name_val));
        exit(1);
    }
    int64_t len;
    const char *bytes = asnix_ptr_bytes(name_val, &len);
    uint32_t sym = asnix_intern(bytes, (size_t)len);
    int64_t idx = asnix_bshared_attrs_find(target, sym);
    if (idx < 0) return 0;
    *out = asnix_force_for_print(asnix_bshared_attrs_value(target, idx));
    return 1;
}

int asnix_bshared_has_attr_dynamic(Value target, Value name_val) {
    if (asnix_tag_of(target) != ASNIX_TAG_ATTRS) return 0;
    if (asnix_tag_of(name_val) != ASNIX_TAG_PTR || asnix_value_is_path(name_val) || asnix_value_is_float(name_val)) {
        fprintf(stderr, "asnix: a dynamic '${...}' attribute name must be a string (got %s)\n",
                asnix_bshared_type_name(name_val));
        exit(1);
    }
    int64_t len;
    const char *bytes = asnix_ptr_bytes(name_val, &len);
    uint32_t sym = asnix_intern(bytes, (size_t)len);
    return asnix_bshared_attrs_find(target, sym) >= 0;
}

int asnix_bshared_value_lt(Value a, Value b, const char *who) {
    a = asnix_force_for_print(a);
    b = asnix_force_for_print(b);
    int ta = asnix_tag_of(a), tb = asnix_tag_of(b);
    if (ta == ASNIX_TAG_INT && tb == ASNIX_TAG_INT) {
        return asnix_int_value(a) < asnix_int_value(b);
    }
    if (ta == ASNIX_TAG_PTR && tb == ASNIX_TAG_PTR && !asnix_value_is_path(a) && !asnix_value_is_path(b) &&
        !asnix_value_is_float(a) && !asnix_value_is_float(b)) {
        int64_t la, lb;
        const char *ba = asnix_ptr_bytes(a, &la);
        const char *bb = asnix_ptr_bytes(b, &lb);
        int64_t n = la < lb ? la : lb;
        int c = n > 0 ? memcmp(ba, bb, (size_t)n) : 0;
        if (c != 0) return c < 0;
        return la < lb;
    }
    /* Lists order lexicographically, recursively comparing elements
     * pairwise (shorter-is-less on a common prefix) -- verified against
     * real `nix-instantiate --eval`: `lessThan [1 2] [1 3]` == true,
     * `lessThan [1] [1 2]` == true, `lessThan [1 2] [1]` == false,
     * `lessThan [] []` == false, equal-length-equal-elements lists are
     * neither less (`lessThan [1 2] [1 2]` == false). A type mismatch
     * between two elements at the first differing index is the same
     * fatal "cannot compare" error as a top-level mismatch (confirmed:
     * `lessThan [1] ["a"]` throws "cannot compare an integer with a
     * string" in real Nix too). */
    if (ta == ASNIX_TAG_LIST && tb == ASNIX_TAG_LIST) {
        int64_t na = asnix_bshared_list_count(a), nb = asnix_bshared_list_count(b);
        const Value *ea = asnix_bshared_list_elems(a), *eb = asnix_bshared_list_elems(b);
        int64_t n = na < nb ? na : nb;
        for (int64_t i = 0; i < n; i++) {
            /* M92: asnix_bshared_value_eq on the RAW (pre-force) elements
             * -- NOT force_for_print'd first, same reasoning as `elem`'s
             * own fix (builtins_list2.c): forcing here would strip
             * exactly the slot/Cell identity asnix_bshared_value_eq's
             * own shortcut needs to tell "two bare-var-aliased elements
             * (e.g. both lists' shared `id`)" apart from "two
             * independently-equal-content-but-unrelated closures" --
             * the former must short-circuit "equal, compare the next
             * element" without ever reaching the fatal "cannot compare
             * these two values" fallback below, the latter correctly
             * falls all the way through to it. Found via
             * numtide/nix-conformance's eval-okay-eq-slot-sort.nix:
             * `lessThan [id 2] [id 1]`, its shared `id` element, used to
             * rely on asnix_bshared_value_eq's old default "pointer-
             * identity" fallback for closures/primops (removed this
             * same milestone, see that function's own comment) --
             * forcing first here would have made `id == id` wrongly
             * fall all the way to "cannot compare". */
            if (asnix_bshared_value_eq(ea[i], eb[i])) continue;
            Value ea_i = asnix_force_for_print(ea[i]);
            Value eb_i = asnix_force_for_print(eb[i]);
            return asnix_bshared_value_lt(ea_i, eb_i, who);
        }
        return na < nb;
    }
    fprintf(stderr, "asnix: %s: cannot compare these two values (only int-vs-int, string-vs-string, and list-vs-list are supported)\n",
            who);
    exit(1);
}

/* M23's asnix_bshared_make_lazy_call1/_call2 (a shared make_thunk_from
 * helper plus two thin wrappers), and M109's later mapAttrs/zipAttrsWith-
 * specific asnix_bshared_make_call2_shared_env/_make_lazy_call2_indexed
 * variant, used to live here -- all removed by M117 once their last
 * callers (map/mapAttrs/zipAttrsWith) were ported to the lighter-weight
 * LazyApp value kind (asnix_make_lazyapp1/_lazyapp2, value_util.c) --
 * see builtins_shared.h's comment at the old declaration site and
 * heap.h's ASNIX_LAZYAPP_MARKER comment. */
