/* M10: prototypes for every AOT op handler, shared by their own
 * definitions and dispatch_table.c's handler table -- one shared
 * declaration keeps every handler's signature textually identical,
 * which musttail requires (the tail-called function's type must match
 * the caller's exactly). */
#ifndef LL_OPS_H
#define LL_OPS_H

#include "../op_dispatch.h"

LL_OP_ATTR Value ll_op_push_int(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_push_true(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_push_false(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_push_null(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_push_const(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_push_int_run(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);

LL_OP_ATTR Value ll_op_add(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_sub(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_mul(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_div(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);

LL_OP_ATTR Value ll_op_eq(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_neq(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_lt(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_gt(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_le(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_ge(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_not(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);

LL_OP_ATTR Value ll_op_eq_if(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_neq_if(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_lt_if(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_ge_if(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);

LL_OP_ATTR Value ll_op_if(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_jmp(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);

LL_OP_ATTR Value ll_op_frame_alloc(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_frame_set(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_frame_exit(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_frame_get(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);

LL_OP_ATTR Value ll_op_var_ref(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_var_ref_raw(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_var_ref_cell(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);

LL_OP_ATTR Value ll_op_lambda(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_mk_thunk(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);

LL_OP_ATTR Value ll_op_call(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_ret(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
/* ll-only, no asnix equivalent, see ir.h's OP_CALL_CHAIN comment. */
LL_OP_ATTR Value ll_op_call_chain(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);

LL_OP_ATTR Value ll_op_mk_list(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_mk_attrs(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_select(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);

LL_OP_ATTR Value ll_op_with(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_with_try(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_with_miss(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_with_exit(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);

LL_OP_ATTR Value ll_op_update(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_concat(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);

LL_OP_ATTR Value ll_op_str_concat(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_path_concat(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);

LL_OP_ATTR Value ll_op_has_attr(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_has_attr_if(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_has_attr_dynamic(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);

LL_OP_ATTR Value ll_op_select_or(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_select_dynamic(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_select_dynamic_or(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);

LL_OP_ATTR Value ll_op_check_formals(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_mk_formalsmap(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs); /* ll-only, see ir.h's OP_MK_FORMALSMAP */
LL_OP_ATTR Value ll_op_assert_fail(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_insert_dynamic_attr(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_rec_overrides_merge(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);
LL_OP_ATTR Value ll_op_resolve_search_path(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);

LL_OP_ATTR Value ll_op_halt(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);

#endif /* LL_OPS_H */
