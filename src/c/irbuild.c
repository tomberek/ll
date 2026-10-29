/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "ll/irbuild.h"

#include <stdlib.h>
#include <string.h>

#include "ll/arena.h"
#include "ll/irpass.h"

IrBuilder *ib_new(void) {
    IrBuilder *b = calloc(1, sizeof(IrBuilder));
    return b;
}

size_t ib_len(IrBuilder *b) {
    return b->len;
}

#ifdef LL_IRDUMP_LINE_TRACKING
/* Compiled in only for irdump's own variant build of this file -- a real
 * production build of irbuild.c never defines this macro. g_current_src_pos
 * is a plain global, not per-builder: compile_expr sets it right before
 * compiling each Ast node, and whichever IrBuilder's ib_emit happens to
 * run next reads it -- correct because compilation is single-threaded
 * and strictly nested (a child program's own builder is fully finished
 * via ib_finish before compile_expr resumes emitting into the parent's). */
static int g_line_tracking_enabled = 0;
static uint32_t g_current_src_pos = 0;

void ib_enable_line_tracking(void) {
    g_line_tracking_enabled = 1;
}

void ib_set_current_src_pos(uint32_t pos) {
    g_current_src_pos = pos;
}

/* Pointer-identity side table from a finished IrProgram to the `lines`
 * array its builder recorded -- see irbuild.h's ll_ib_lookup_program_
 * lines comment for why this is a side table and not a field on
 * IrProgram itself. */
#define LL_IB_MAX_LINE_PROGRAMS 65536
static const IrProgram *g_line_programs[LL_IB_MAX_LINE_PROGRAMS];
static const uint32_t *g_line_program_lines[LL_IB_MAX_LINE_PROGRAMS];
static size_t g_nline_programs = 0;

const uint32_t *ll_ib_lookup_program_lines(const IrProgram *prog) {
    for (size_t i = 0; i < g_nline_programs; i++) {
        if (g_line_programs[i] == prog) return g_line_program_lines[i];
    }
    return NULL;
}
#endif /* LL_IRDUMP_LINE_TRACKING */

static size_t ib_emit(IrBuilder *b, int op_id, int64_t a, int64_t bval) {
    if (b->len == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 24;
        b->ops = realloc(b->ops, b->cap * sizeof(IrOp));
    }
    b->ops[b->len].handler = (void *)(intptr_t)op_id;
    b->ops[b->len].a = a;
    b->ops[b->len].b = bval;
#ifdef LL_IRDUMP_LINE_TRACKING
    if (g_line_tracking_enabled) {
        if (b->len == b->lines_cap) {
            b->lines_cap = b->lines_cap ? b->lines_cap * 2 : 24;
            b->lines = realloc(b->lines, b->lines_cap * sizeof(uint32_t));
        }
        b->lines[b->len] = g_current_src_pos;
    }
#endif
    return b->len++;
}

/* Forward-declared: defined below, next to the rest of the pool-management
 * helpers -- ib_push_int needs it since OP_PUSH_INT routes its literal
 * through the pool instead of carrying it inline. */
static size_t ib_pool_add(IrBuilder *b, int64_t v);

size_t ib_push_int(IrBuilder *b, int64_t v) {
    /* OP_PUSH_INT routes its literal through the constant pool exactly
     * like ib_push_const, rather than carrying it inline: IrOp.a is
     * only 32 bits, too narrow for an arbitrary int64 Nix literal. */
    size_t pool_idx = ib_pool_add(b, v);
    return ib_emit(b, OP_PUSH_INT, 0, (int64_t)pool_idx);
}
size_t ib_push_true(IrBuilder *b) {
    return ib_emit(b, OP_PUSH_TRUE, 0, 0);
}
size_t ib_push_false(IrBuilder *b) {
    return ib_emit(b, OP_PUSH_FALSE, 0, 0);
}
size_t ib_push_null(IrBuilder *b) {
    return ib_emit(b, OP_PUSH_NULL, 0, 0);
}
size_t ib_var_ref(IrBuilder *b, int64_t depth, int64_t slot) {
    return ib_emit(b, OP_VAR_REF, depth, slot);
}
size_t ib_var_ref_cell(IrBuilder *b, int64_t depth, int64_t slot) {
    return ib_emit(b, OP_VAR_REF_CELL, depth, slot);
}
size_t ib_var_ref_raw(IrBuilder *b, int64_t depth, int64_t slot) {
    return ib_emit(b, OP_VAR_REF_RAW, depth, slot);
}
size_t ib_add(IrBuilder *b) {
    return ib_emit(b, OP_ADD, 0, 0);
}
size_t ib_sub(IrBuilder *b) {
    return ib_emit(b, OP_SUB, 0, 0);
}
size_t ib_mul(IrBuilder *b) {
    return ib_emit(b, OP_MUL, 0, 0);
}
size_t ib_if(IrBuilder *b) {
    return ib_emit(b, OP_IF, 0, 0);
}
size_t ib_jmp(IrBuilder *b) {
    return ib_emit(b, OP_JMP, 0, 0);
}

void ib_patch_delta(IrBuilder *b, size_t site, size_t target_idx) {
    b->ops[site].a = (int64_t)(target_idx - site) * LL_IROP_SIZE;
}

size_t ib_let(IrBuilder *b, int64_t nslots) {
    return ib_emit(b, OP_LET, nslots, 0);
}

static size_t ib_pool_add(IrBuilder *b, int64_t v) {
    if (b->pool_len == b->pool_cap) {
        b->pool_cap = b->pool_cap ? b->pool_cap * 2 : 24;
        b->pool = realloc(b->pool, b->pool_cap * sizeof(int64_t));
    }
    b->pool[b->pool_len] = v;
    return b->pool_len++;
}

size_t ib_lambda(IrBuilder *b, int64_t arity, IrProgram *body_prog, int64_t formals_raw) {
    /* A single IrProgram* pool entry, not a {ops, pool} pair -- see
     * ir.h's LL_CLOSURE_OFF_PROG comment. */
    size_t pool_idx = ib_pool_add(b, (int64_t)(intptr_t)body_prog);
    ib_pool_add(b, formals_raw); /* always pool_idx+1, see op_lambda.S */
    return ib_emit(b, OP_LAMBDA, arity, (int64_t)pool_idx);
}

size_t ib_call(IrBuilder *b, int64_t argc) {
    return ib_emit(b, OP_CALL, argc, 0);
}

/* ll-only, no asnix equivalent: see ir.h's OP_CALL_CHAIN comment. */
size_t ib_call_chain(IrBuilder *b, int64_t n) {
    return ib_emit(b, OP_CALL_CHAIN, n, 0);
}
size_t ib_ret(IrBuilder *b) {
    return ib_emit(b, OP_RET, 0, 0);
}
size_t ib_halt(IrBuilder *b) {
    return ib_emit(b, OP_HALT, 0, 0);
}

size_t ib_push_const(IrBuilder *b, int64_t raw_value) {
    size_t pool_idx = ib_pool_add(b, raw_value);
    return ib_emit(b, OP_PUSH_CONST, 0, (int64_t)pool_idx);
}

size_t ib_mk_list(IrBuilder *b, int64_t count) {
    return ib_emit(b, OP_MK_LIST, count, 0);
}

size_t ib_mk_attrs(IrBuilder *b, int64_t count, const uint32_t *sorted_syms) {
    size_t pool_idx = (size_t)-1;
    for (int64_t i = 0; i < count; i++) {
        size_t idx = ib_pool_add(b, (int64_t)sorted_syms[i]);
        if (i == 0) pool_idx = idx;
    }
    if (count == 0) pool_idx = ib_pool_add(b, 0); /* keep `b` a valid index even for {} */
    return ib_emit(b, OP_MK_ATTRS, count, (int64_t)pool_idx);
}

size_t ib_select(IrBuilder *b, uint32_t sym) {
    return ib_emit(b, OP_SELECT, (int64_t)sym, 0);
}

size_t ib_select_dynamic(IrBuilder *b) {
    return ib_emit(b, OP_SELECT_DYNAMIC, 0, 0);
}

size_t ib_mk_thunk(IrBuilder *b, IrProgram *body_prog) {
    /* A single IrProgram* pool entry, not a {ops, pool} pair -- see
     * ir.h's LL_THUNK_OFF_STATE comment (the runtime Thunk cell's own
     * prog field is packed into that same word, not a separate one,
     * but this compile-time IR pool entry is unaffected). */
    size_t pool_idx = ib_pool_add(b, (int64_t)(intptr_t)body_prog);
    return ib_emit(b, OP_MK_THUNK, 0, (int64_t)pool_idx);
}

/* Emits the single OP_LAZY_THUNK_BODY op that makes up a whole stub
 * IrProgram -- `desc` is a LazyThunkDescriptor* (compiler.c), taken
 * here as an opaque `void*` so this header and irbuild.c don't need to
 * know that type's layout. */
size_t ib_mk_lazy_thunk_stub(IrBuilder *b, void *desc) {
    size_t pool_idx = ib_pool_add(b, (int64_t)(intptr_t)desc);
    return ib_emit(b, OP_LAZY_THUNK_BODY, 0, (int64_t)pool_idx);
}

size_t ib_frame_alloc(IrBuilder *b, int64_t nslots) {
    return ib_emit(b, OP_FRAME_ALLOC, nslots, 0);
}

size_t ib_frame_set(IrBuilder *b, int64_t slot) {
    return ib_emit(b, OP_FRAME_SET, slot, 0);
}

size_t ib_frame_get(IrBuilder *b, int64_t slot) {
    return ib_emit(b, OP_FRAME_GET, slot, 0);
}

size_t ib_frame_exit(IrBuilder *b) {
    return ib_emit(b, OP_FRAME_EXIT, 0, 0);
}

size_t ib_rec_overrides_merge(IrBuilder *b, int64_t n, const uint32_t *syms, int64_t overrides_slot) {
    size_t pool_idx = (size_t)-1;
    for (int64_t i = 0; i < n; i++) {
        size_t idx = ib_pool_add(b, (int64_t)syms[i]);
        if (i == 0) pool_idx = idx;
    }
    size_t slot_idx = ib_pool_add(b, overrides_slot);
    if (n == 0) pool_idx = slot_idx; /* n == 0 can't actually happen (there's always at
                                       * least the __overrides binding itself), but keep
                                       * `b` valid regardless, same defensive habit
                                       * ib_mk_attrs's own count==0 case has. */
    return ib_emit(b, OP_REC_OVERRIDES_MERGE, n, (int64_t)pool_idx);
}

size_t ib_eq(IrBuilder *b) {
    return ib_emit(b, OP_EQ, 0, 0);
}
size_t ib_neq(IrBuilder *b) {
    return ib_emit(b, OP_NEQ, 0, 0);
}
size_t ib_lt(IrBuilder *b) {
    return ib_emit(b, OP_LT, 0, 0);
}
size_t ib_gt(IrBuilder *b) {
    return ib_emit(b, OP_GT, 0, 0);
}
size_t ib_le(IrBuilder *b) {
    return ib_emit(b, OP_LE, 0, 0);
}
size_t ib_ge(IrBuilder *b) {
    return ib_emit(b, OP_GE, 0, 0);
}
size_t ib_not(IrBuilder *b) {
    return ib_emit(b, OP_NOT, 0, 0);
}
size_t ib_update(IrBuilder *b) {
    return ib_emit(b, OP_UPDATE, 0, 0);
}
size_t ib_concat(IrBuilder *b) {
    return ib_emit(b, OP_CONCAT, 0, 0);
}
size_t ib_div(IrBuilder *b) {
    return ib_emit(b, OP_DIV, 0, 0);
}
size_t ib_insert_dynamic_attr(IrBuilder *b) {
    return ib_emit(b, OP_INSERT_DYNAMIC_ATTR, 0, 0);
}
size_t ib_select_dynamic_or(IrBuilder *b) {
    return ib_emit(b, OP_SELECT_DYNAMIC_OR, 0, 0);
}
size_t ib_resolve_search_path(IrBuilder *b) {
    return ib_emit(b, OP_RESOLVE_SEARCH_PATH, 0, 0);
}

size_t ib_assert_fail(IrBuilder *b, const char *msg, size_t len) {
    size_t pool_idx = ib_pool_add(b, (int64_t)(intptr_t)msg);
    ib_pool_add(b, (int64_t)len); /* always pool_idx+1, see op_assert.S */
    return ib_emit(b, OP_ASSERT_FAIL, 0, (int64_t)pool_idx);
}

size_t ib_str_concat(IrBuilder *b, int64_t count) {
    return ib_emit(b, OP_STR_CONCAT, count, 0);
}

size_t ib_path_concat(IrBuilder *b, int64_t count) {
    return ib_emit(b, OP_PATH_CONCAT, count, 0);
}

size_t ib_with(IrBuilder *b) {
    return ib_emit(b, OP_WITH, 0, 0);
}

size_t ib_with_try(IrBuilder *b, uint32_t sym) {
    return ib_emit(b, OP_WITH_TRY, 0, (int64_t)sym);
}

size_t ib_with_miss(IrBuilder *b) {
    return ib_emit(b, OP_WITH_MISS, 0, 0);
}

size_t ib_with_exit(IrBuilder *b) {
    return ib_emit(b, OP_WITH_EXIT, 0, 0);
}

size_t ib_has_attr(IrBuilder *b, uint32_t sym) {
    return ib_emit(b, OP_HAS_ATTR, (int64_t)sym, 0);
}

size_t ib_has_attr_dynamic(IrBuilder *b) {
    return ib_emit(b, OP_HAS_ATTR_DYNAMIC, 0, 0);
}

size_t ib_check_formals(IrBuilder *b, int64_t required_count, const uint32_t *required_syms_sorted,
                         int64_t total_count, const uint32_t *all_syms_sorted) {
    /* Pool layout -- see op_check_formals.S's long comment. */
    size_t pool_idx = ib_pool_add(b, total_count); /* -1 means "has `...`, skip the extra-field check" */
    for (int64_t i = 0; i < required_count; i++) ib_pool_add(b, (int64_t)required_syms_sorted[i]);
    if (total_count >= 0) {
        for (int64_t i = 0; i < total_count; i++) ib_pool_add(b, (int64_t)all_syms_sorted[i]);
    }
    return ib_emit(b, OP_CHECK_FORMALS, required_count, (int64_t)pool_idx);
}

/* ll-only, no asnix equivalent, see ir.h's OP_MK_FORMALSMAP comment:
 * `syms[i]` is the formal at POSITION i's symbol id if it uses this
 * mechanism (a required formal, no default), or -1 if it does not (an
 * optional-with-default formal, left to the ordinary per-formal thunk
 * path the caller still emits separately). */
size_t ib_mk_formalsmap(IrBuilder *b, int64_t nf, const int64_t *syms) {
    size_t pool_idx = (size_t)-1;
    for (int64_t i = 0; i < nf; i++) {
        size_t idx = ib_pool_add(b, syms[i]);
        if (i == 0) pool_idx = idx;
    }
    return ib_emit(b, OP_MK_FORMALSMAP, nf, (int64_t)pool_idx);
}

size_t ib_select_or(IrBuilder *b, uint32_t sym) {
    return ib_emit(b, OP_SELECT_OR, 0, (int64_t)sym);
}

size_t ib_eq_if(IrBuilder *b) {
    return ib_emit(b, OP_EQ_IF, 0, 0);
}
size_t ib_neq_if(IrBuilder *b) {
    return ib_emit(b, OP_NEQ_IF, 0, 0);
}
size_t ib_lt_if(IrBuilder *b) {
    return ib_emit(b, OP_LT_IF, 0, 0);
}
size_t ib_ge_if(IrBuilder *b) {
    return ib_emit(b, OP_GE_IF, 0, 0);
}
size_t ib_has_attr_if(IrBuilder *b, uint32_t sym) {
    return ib_emit(b, OP_HAS_ATTR_IF, 0, (int64_t)sym);
}

IrProgram *ib_finish(IrBuilder *b) {
    /* Opt-in bytecode-to-bytecode pass pipeline (ll/irpass.h) -- a
     * no-op (0 passes registered) in every normal build/run; see that
     * header's own comment. `&b->pool`/`&b->pool_len`/`&b->pool_cap`
     * are threaded through by pointer so a registered pass can both
     * read an existing pool entry's Value and append a freshly-built
     * one -- this IS the live builder's own pool, still growable at
     * this point (ib_finish's own pool-shrinking realloc below runs
     * AFTER this call). */
    ll_irpass_begin_program();
#ifdef LL_IRDUMP_LINE_TRACKING
    /* Lets ll_irpass_remove's existing shift logic keep `b->lines`
     * aligned with `ops` across a pass's removal, exactly the way it
     * already keeps the firing log and jump targets aligned. Cleared
     * again right after, so a later, unrelated ib_finish call (irdump
     * re-entering lex_parse_compile_dump for its own nested child
     * programs) never sees a stale pointer. Entirely absent from a real
     * production build -- see irbuild.h's own comment. */
    ll_irpass_set_line_array(g_line_tracking_enabled ? b->lines : NULL);
#endif
    ll_irpass_run(b->ops, &b->len, &b->pool, &b->pool_len, &b->pool_cap);
#ifdef LL_IRDUMP_LINE_TRACKING
    ll_irpass_set_line_array(NULL);
#endif
    ir_link(b->ops, b->len);
    /* ops/pool are handed to `prog`, which lives for the rest of the
     * process (every compiled function/thunk keeps its own IrProgram
     * forever) -- but ib_emit/ib_pool_add grow them by doubling, so
     * whichever one lands at e.g. 55% full right before ib_finish
     * carries that ~45% of slack permanently. Trimming to the exact
     * final size here (safe: realloc to a smaller size never moves/
     * invalidates the still-needed prefix) removes that slack for good.
     *
     * The trimmed copy is routed through `ll_arena_alloc` rather than a
     * plain `realloc()`: shrinking from the generous 24-slot initial
     * guess down to the real (usually much smaller) final size tends to
     * cross a mimalloc size-class boundary, so `realloc()` down is not
     * a cheap in-place truncation -- it's a real alloc-new+copy+free-old,
     * and the result is never freed anyway (ib_finish hands `ops`/`pool`
     * straight to an IrProgram that lives for the rest of the process).
     * Routing through the arena's bump allocator instead avoids that
     * realloc cost entirely; the old oversized scratch buffer is then
     * `free()`d directly. The `b->len == b->cap` (no slack) case is
     * untouched: there is nothing to move to the arena there. */
    if (b->len > 0 && b->len < b->cap) {
        size_t bytes = b->len * sizeof(IrOp);
        IrOp *shrunk = ll_arena_alloc(bytes);
        memcpy(shrunk, b->ops, bytes);
        free(b->ops);
        b->ops = shrunk;
    }
    if (b->pool_len > 0 && b->pool_len < b->pool_cap) {
        size_t bytes = b->pool_len * sizeof(int64_t);
        int64_t *shrunk = ll_arena_alloc(bytes);
        memcpy(shrunk, b->pool, bytes);
        free(b->pool);
        b->pool = shrunk;
    }
    IrProgram *prog = calloc(1, sizeof(IrProgram));
    prog->ops = b->ops;
    prog->pool = b->pool;
    ll_irpass_mark_program(prog);
#ifdef LL_IRDUMP_LINE_TRACKING
    /* Register (prog, lines) in the side table irdump looks up by pointer
     * identity (ll_ib_lookup_program_lines, irbuild.h). Deliberately
     * not routed through the arena shrink-to-fit above: this is a
     * one-shot debug CLI tool's own scratch memory, not a cost a real
     * long-running eval ever pays (this whole block is absent from a
     * real production build), so there is no permanent-footprint reason
     * to bother. */
    if (g_line_tracking_enabled && b->lines && g_nline_programs < LL_IB_MAX_LINE_PROGRAMS) {
        g_line_programs[g_nline_programs] = prog;
        g_line_program_lines[g_nline_programs] = b->lines;
        g_nline_programs++;
    }
#endif
    /* ops/pool were just pulled out into `prog` above -- nothing else
     * references the outer IrBuilder struct itself afterward (every
     * caller discards its local builder variable immediately after
     * this call), so it must be freed here or it leaks on every call. */
    free(b);
    return prog;
}
