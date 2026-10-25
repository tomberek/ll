/* M10: replaces backend_stub.c's no-op ir_link with a real one,
 * mapping each op-id (as emitted by the builder) to the address of
 * this backend's own AOT-compiled C handler (src/llvm/ops/*.c) --
 * the exact same role asnix's own dispatch_table.c plays for its
 * hand-written asm handlers, just pointing at different code.
 *
 * ib_finish() (src/c/irbuild.c, vendored unchanged) already calls
 * ir_link() unconditionally on every IrProgram it ever builds -- the
 * top-level program, every lambda/thunk body, and every curried-
 * native-builtin wrapper -- so this one file is the entire linking
 * story; no other call site needs to change. */
#include <stdio.h>
#include <stdlib.h>

#include "ops/ops.h"

void *ll_handler_table[OP_COUNT] = {
    [OP_PUSH_INT] = (void *)ll_op_push_int,
    [OP_PUSH_TRUE] = (void *)ll_op_push_true,
    [OP_PUSH_FALSE] = (void *)ll_op_push_false,
    [OP_PUSH_NULL] = (void *)ll_op_push_null,
    [OP_PUSH_CONST] = (void *)ll_op_push_const,

    [OP_ADD] = (void *)ll_op_add,
    [OP_SUB] = (void *)ll_op_sub,
    [OP_MUL] = (void *)ll_op_mul,
    [OP_DIV] = (void *)ll_op_div,

    [OP_EQ] = (void *)ll_op_eq,
    [OP_NEQ] = (void *)ll_op_neq,
    [OP_LT] = (void *)ll_op_lt,
    [OP_GT] = (void *)ll_op_gt,
    [OP_LE] = (void *)ll_op_le,
    [OP_GE] = (void *)ll_op_ge,
    [OP_NOT] = (void *)ll_op_not,

    [OP_EQ_IF] = (void *)ll_op_eq_if,
    [OP_NEQ_IF] = (void *)ll_op_neq_if,
    [OP_LT_IF] = (void *)ll_op_lt_if,
    [OP_GE_IF] = (void *)ll_op_ge_if,

    [OP_IF] = (void *)ll_op_if,
    [OP_JMP] = (void *)ll_op_jmp,

    [OP_FRAME_ALLOC] = (void *)ll_op_frame_alloc,
    [OP_FRAME_SET] = (void *)ll_op_frame_set,
    [OP_FRAME_EXIT] = (void *)ll_op_frame_exit,
    [OP_FRAME_GET] = (void *)ll_op_frame_get,

    [OP_VAR_REF] = (void *)ll_op_var_ref,
    [OP_VAR_REF_RAW] = (void *)ll_op_var_ref_raw,
    [OP_VAR_REF_CELL] = (void *)ll_op_var_ref_cell,

    [OP_LAMBDA] = (void *)ll_op_lambda,
    [OP_MK_THUNK] = (void *)ll_op_mk_thunk,

    [OP_CALL] = (void *)ll_op_call,
    [OP_RET] = (void *)ll_op_ret,

    [OP_MK_LIST] = (void *)ll_op_mk_list,
    [OP_MK_ATTRS] = (void *)ll_op_mk_attrs,
    [OP_SELECT] = (void *)ll_op_select,

    [OP_HALT] = (void *)ll_op_halt,
};

/* M10 perf: a post-link peephole pass, not a compiler.c change --
 * compiler.c (vendored, byte-identical to asnix) stays untouched, and
 * nothing it emits knows this pass exists. A literal list/attrset
 * built entirely from plain integer constants (list_build_large.nix,
 * attrset_build_large.nix) compiles to a long run of bare
 * OP_PUSH_INT with nothing interleaved, because `ib_pool_add`
 * allocates pool slots in strict call order: run detection just
 * needs "N consecutive OP_PUSH_INT records whose pool indices are
 * consecutive integers," checked while `handler` still holds the
 * small op-id (before the main linking loop below overwrites it).
 * Runs of 4 or more get flagged (`a` repurposed to hold the run
 * length -- always 0 on a lone OP_PUSH_INT, see ib_push_int) so the
 * main loop links the run's first record to ll_op_push_int_run
 * instead of the ordinary one-push-per-hop handler; the remaining
 * records in the run still link normally and stay individually
 * valid, just unreached in the ordinary control flow (the fused
 * handler's own ip += count skips straight over them). */
#define LL_PUSH_INT_RUN_MIN 4

static void fuse_push_int_runs(IrOp *ops, size_t n) {
    size_t i = 0;
    while (i < n) {
        if ((intptr_t)ops[i].handler != OP_PUSH_INT) {
            i++;
            continue;
        }
        size_t j = i + 1;
        while (j < n && (intptr_t)ops[j].handler == OP_PUSH_INT && ops[j].b == ops[j - 1].b + 1) j++;
        size_t run_len = j - i;
        if (run_len >= LL_PUSH_INT_RUN_MIN) ops[i].a = (int32_t)run_len;
        i = j;
    }
}

void ir_link(IrOp *ops, size_t n) {
    fuse_push_int_runs(ops, n);
    for (size_t i = 0; i < n; i++) {
        intptr_t op_id = (intptr_t)ops[i].handler;
        void *h;
        if (op_id == OP_PUSH_INT && ops[i].a > 0) {
            h = (void *)ll_op_push_int_run;
        } else {
            h = (op_id >= 0 && op_id < OP_COUNT) ? ll_handler_table[op_id] : NULL;
        }
        if (!h) {
            fprintf(stderr, "ll: op id %ld has no AOT handler (not yet ported), aborting\n", (long)op_id);
            exit(1);
        }
        ops[i].handler = h;
    }
}
