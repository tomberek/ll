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

    [OP_WITH] = (void *)ll_op_with,
    [OP_WITH_TRY] = (void *)ll_op_with_try,
    [OP_WITH_MISS] = (void *)ll_op_with_miss,
    [OP_WITH_EXIT] = (void *)ll_op_with_exit,

    [OP_UPDATE] = (void *)ll_op_update,
    [OP_CONCAT] = (void *)ll_op_concat,

    [OP_STR_CONCAT] = (void *)ll_op_str_concat,
    [OP_PATH_CONCAT] = (void *)ll_op_path_concat,

    [OP_HAS_ATTR] = (void *)ll_op_has_attr,
    [OP_HAS_ATTR_IF] = (void *)ll_op_has_attr_if,
    [OP_HAS_ATTR_DYNAMIC] = (void *)ll_op_has_attr_dynamic,

    [OP_SELECT_OR] = (void *)ll_op_select_or,
    [OP_SELECT_DYNAMIC] = (void *)ll_op_select_dynamic,
    [OP_SELECT_DYNAMIC_OR] = (void *)ll_op_select_dynamic_or,

    [OP_CHECK_FORMALS] = (void *)ll_op_check_formals,
    [OP_ASSERT_FAIL] = (void *)ll_op_assert_fail,
    [OP_INSERT_DYNAMIC_ATTR] = (void *)ll_op_insert_dynamic_attr,
    [OP_REC_OVERRIDES_MERGE] = (void *)ll_op_rec_overrides_merge,
    [OP_RESOLVE_SEARCH_PATH] = (void *)ll_op_resolve_search_path,

    [OP_HALT] = (void *)ll_op_halt,
};

/* M10 perf: a single forward-pass link + peephole fusion, not a
 * compiler.c change -- compiler.c (vendored, byte-identical to
 * asnix) stays untouched, and nothing it emits knows this fusion
 * exists. A literal list/attrset built entirely from plain integer
 * constants (list_build_large.nix, attrset_build_large.nix) compiles
 * to a long run of bare OP_PUSH_INT with nothing interleaved,
 * because `ib_pool_add` allocates pool slots in strict call order:
 * run detection just needs "N consecutive OP_PUSH_INT records whose
 * pool indices are consecutive integers," checked while `handler`
 * still holds the small op-id for the not-yet-visited lookahead
 * records -- safe because this loop only ever writes ops[i] (and,
 * for a fused run, the already-looked-ahead ops[i+1..j-1]), never
 * reads ahead into anything this same pass has already linked.
 * Runs of 4 or more get the first record linked straight to
 * ll_op_push_int_run (with `a` repurposed to hold the run length);
 * every other record in the run still gets linked to the ordinary
 * ll_op_push_int so it stays individually valid, just unreached in
 * the ordinary control flow (the fused handler's own ip += count
 * skips straight over them). This used to be two full passes over
 * `ops` (a separate fuse_push_int_runs pre-pass, then this linking
 * loop); merging them into one removes a full redundant scan of
 * every op in every compiled function body. */
#define LL_PUSH_INT_RUN_MIN 4

/* No bounds check, no null-handler fatal: matches asnix's own
 * ir_link exactly (src/c/dispatch_table.c there), which trusts its
 * compiler to never emit an op id outside the handler table -- a
 * trust this backend has now earned the same way (M6/M7: every op
 * the compiler can emit has an AOT handler, confirmed against the
 * full real-nixpkgs correctness sweep). This dropped the two checks
 * that used to run on every op of every compiled function body,
 * closing roughly a third of ir_link's instruction-count gap to
 * asnix's own version -- see M8-findings.md's own section on this. */
void ir_link(IrOp *ops, size_t n) {
    for (size_t i = 0; i < n;) {
        intptr_t op_id = (intptr_t)ops[i].handler;
        if (op_id != OP_PUSH_INT) {
            ops[i].handler = ll_handler_table[op_id];
            i++;
            continue;
        }
        size_t j = i + 1;
        while (j < n && (intptr_t)ops[j].handler == OP_PUSH_INT && ops[j].b == ops[j - 1].b + 1) j++;
        size_t run_len = j - i;
        if (run_len >= LL_PUSH_INT_RUN_MIN) {
            for (size_t k = i + 1; k < j; k++) ops[k].handler = ll_handler_table[OP_PUSH_INT];
            ops[i].handler = (void *)ll_op_push_int_run;
            ops[i].a = (int32_t)run_len;
            i = j;
        } else {
            ops[i].handler = ll_handler_table[OP_PUSH_INT];
            i++;
        }
    }
}
