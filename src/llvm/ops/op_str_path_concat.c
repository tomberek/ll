/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445's src/asm/ops/op_str_concat.S
 * and src/asm/ops/op_path_concat.S -- see soft-wishing-engelbart.md plan
 *
 * Both ops are variable-arity (a = count): pop `count` values already
 * contiguous on VALSTACK, hand a pointer straight at them to one
 * vendored C helper, push the single result. OP_PATH_CONCAT is byte-
 * identical to OP_STR_CONCAT except for which helper it calls:
 * ll_concat_path (value_util.c) re-canonicalizes the concatenated text
 * and re-wraps it as a Path, which is only correct because compiler.c's
 * AST_PATH case guarantees `parts[0]` is already a Path constant
 * whenever it emits this op -- no runtime check of the first operand
 * is needed or done here. */
#include "ops.h"
#include "value_util.h"

LL_OP_ATTR Value ll_op_str_concat(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    int64_t count = ip->a;
    Value *parts = vs - count;
    Value result = ll_concat_strings(parts, count);
    vs = parts;
    ll_check_valstack(vs);
    vs[0] = result;
    vs++;
    LL_NEXT(ip, env, vs, pool, rs);
}

LL_OP_ATTR Value ll_op_path_concat(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    int64_t count = ip->a;
    Value *parts = vs - count;
    Value result = ll_concat_path(parts, count);
    vs = parts;
    ll_check_valstack(vs);
    vs[0] = result;
    vs++;
    LL_NEXT(ip, env, vs, pool, rs);
}
