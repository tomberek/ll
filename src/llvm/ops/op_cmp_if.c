/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445's src/asm/ops/op_cmp_if.S -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "ops.h"
#include "value_util.h"
#include "builtins_shared.h"

Value ll_op_eq_if(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    Value rhs = vs[-1], lhs = vs[-2];
    vs -= 2;
    int eq = asnix_bshared_value_eq_op(lhs, rhs);
    if (!eq) {
        ip = (IrOp *)((char *)ip + ip->a);
    } else {
        ip++;
    }
    LL_GOTO(ip, env, vs, pool, rs);
}

Value ll_op_neq_if(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    Value rhs = vs[-1], lhs = vs[-2];
    vs -= 2;
    int eq = asnix_bshared_value_eq_op(lhs, rhs);
    if (eq) {
        ip = (IrOp *)((char *)ip + ip->a);
    } else {
        ip++;
    }
    LL_GOTO(ip, env, vs, pool, rs);
}

Value ll_op_lt_if(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    Value rhs = vs[-1], lhs = vs[-2];
    vs -= 2;
    int take_then;
    if ((lhs.raw & ASNIX_TAG_MASK) == ASNIX_TAG_INT && (rhs.raw & ASNIX_TAG_MASK) == ASNIX_TAG_INT) {
        int64_t l = (int64_t)lhs.raw >> ASNIX_TAG_BITS;
        int64_t r = (int64_t)rhs.raw >> ASNIX_TAG_BITS;
        take_then = (l < r);
    } else {
        Value res = asnix_cmp_lt_values(lhs, rhs);
        take_then = (res.raw == ll_true().raw);
    }
    if (take_then) {
        ip++;
    } else {
        ip = (IrOp *)((char *)ip + ip->a);
    }
    LL_GOTO(ip, env, vs, pool, rs);
}

Value ll_op_ge_if(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    Value rhs = vs[-1], lhs = vs[-2];
    vs -= 2;
    int take_then;
    if ((lhs.raw & ASNIX_TAG_MASK) == ASNIX_TAG_INT && (rhs.raw & ASNIX_TAG_MASK) == ASNIX_TAG_INT) {
        int64_t l = (int64_t)lhs.raw >> ASNIX_TAG_BITS;
        int64_t r = (int64_t)rhs.raw >> ASNIX_TAG_BITS;
        take_then = (l >= r);
    } else {
        Value res = asnix_cmp_ge_values(lhs, rhs);
        take_then = (res.raw == ll_true().raw);
    }
    if (take_then) {
        ip++;
    } else {
        ip = (IrOp *)((char *)ip + ip->a);
    }
    LL_GOTO(ip, env, vs, pool, rs);
}
