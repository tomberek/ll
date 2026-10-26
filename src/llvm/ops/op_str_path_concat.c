/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445's src/asm/ops/op_str_concat.S
 * and src/asm/ops/op_path_concat.S -- see soft-wishing-engelbart.md plan
 *
 * Both ops are variable-arity (a = count): pop `count` values already
 * contiguous on VALSTACK, hand a pointer straight at them to one
 * vendored C helper, push the single result -- same pop-N-build-one
 * shape op_mk_list.c's ll_op_mk_list already uses. OP_PATH_CONCAT is
 * byte-identical to OP_STR_CONCAT except for which helper it calls:
 * asnix_concat_path (value_util.c) internally re-canonicalizes the
 * concatenated text and re-wraps it as a Path, which is only correct
 * because compiler.c's AST_PATH case guarantees `parts[0]` is already
 * a Path constant whenever it emits this op (see OP_PATH_CONCAT's own
 * comment in ir.h) -- no runtime check of the first operand is needed
 * or done here, matching the asm, which calls asnix_concat_path
 * unconditionally. */
#include "ops.h"
#include "value_util.h"

LL_OP_ATTR Value ll_op_str_concat(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    int64_t count = ip->a;
    Value *parts = vs - count; /* the popped values already sit contiguously here */
    Value result = asnix_concat_strings(parts, count);
    vs = parts;
    ll_check_valstack(vs);
    vs[0] = result;
    vs++;
    LL_NEXT(ip, env, vs, pool, rs);
}

LL_OP_ATTR Value ll_op_path_concat(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    int64_t count = ip->a;
    Value *parts = vs - count; /* the popped values already sit contiguously here */
    Value result = asnix_concat_path(parts, count);
    vs = parts;
    ll_check_valstack(vs);
    vs[0] = result;
    vs++;
    LL_NEXT(ip, env, vs, pool, rs);
}
