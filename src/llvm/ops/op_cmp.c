/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445's src/asm/ops/op_cmp.S -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "ops.h"
#include "value_util.h"
#include "builtins_shared.h"

LL_OP_ATTR Value ll_op_eq(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    Value rhs = vs[-1], lhs = vs[-2];
    int eq = ll_bshared_value_eq_op(lhs, rhs);
    vs--;
    vs[-1] = eq ? ll_true() : ll_false();
    LL_NEXT(ip, env, vs, pool, rs);
}

LL_OP_ATTR Value ll_op_neq(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    Value rhs = vs[-1], lhs = vs[-2];
    int eq = ll_bshared_value_eq_op(lhs, rhs);
    vs--;
    vs[-1] = eq ? ll_false() : ll_true();
    LL_NEXT(ip, env, vs, pool, rs);
}

LL_OP_ATTR Value ll_op_lt(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    Value rhs = vs[-1], lhs = vs[-2];
    Value result;
    if ((lhs.raw & LL_TAG_MASK) == LL_TAG_INT && (rhs.raw & LL_TAG_MASK) == LL_TAG_INT) {
        int64_t l = (int64_t)lhs.raw >> LL_TAG_BITS;
        int64_t r = (int64_t)rhs.raw >> LL_TAG_BITS;
        result = (l < r) ? ll_true() : ll_false();
    } else {
        result = ll_cmp_lt_values(lhs, rhs);
    }
    vs--;
    vs[-1] = result;
    LL_NEXT(ip, env, vs, pool, rs);
}

LL_OP_ATTR Value ll_op_gt(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    Value rhs = vs[-1], lhs = vs[-2];
    Value result;
    if ((lhs.raw & LL_TAG_MASK) == LL_TAG_INT && (rhs.raw & LL_TAG_MASK) == LL_TAG_INT) {
        int64_t l = (int64_t)lhs.raw >> LL_TAG_BITS;
        int64_t r = (int64_t)rhs.raw >> LL_TAG_BITS;
        result = (l > r) ? ll_true() : ll_false();
    } else {
        result = ll_cmp_gt_values(lhs, rhs);
    }
    vs--;
    vs[-1] = result;
    LL_NEXT(ip, env, vs, pool, rs);
}

LL_OP_ATTR Value ll_op_le(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    Value rhs = vs[-1], lhs = vs[-2];
    Value result;
    if ((lhs.raw & LL_TAG_MASK) == LL_TAG_INT && (rhs.raw & LL_TAG_MASK) == LL_TAG_INT) {
        int64_t l = (int64_t)lhs.raw >> LL_TAG_BITS;
        int64_t r = (int64_t)rhs.raw >> LL_TAG_BITS;
        result = (l <= r) ? ll_true() : ll_false();
    } else {
        result = ll_cmp_le_values(lhs, rhs);
    }
    vs--;
    vs[-1] = result;
    LL_NEXT(ip, env, vs, pool, rs);
}

LL_OP_ATTR Value ll_op_ge(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    Value rhs = vs[-1], lhs = vs[-2];
    Value result;
    if ((lhs.raw & LL_TAG_MASK) == LL_TAG_INT && (rhs.raw & LL_TAG_MASK) == LL_TAG_INT) {
        int64_t l = (int64_t)lhs.raw >> LL_TAG_BITS;
        int64_t r = (int64_t)rhs.raw >> LL_TAG_BITS;
        result = (l >= r) ? ll_true() : ll_false();
    } else {
        result = ll_cmp_ge_values(lhs, rhs);
    }
    vs--;
    vs[-1] = result;
    LL_NEXT(ip, env, vs, pool, rs);
}

LL_OP_ATTR Value ll_op_not(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    Value v = vs[-1];
    if ((v.raw & LL_TAG_MASK) != LL_TAG_IMM) goto type_error;
    {
        uint64_t sub = v.raw >> LL_TAG_BITS;
        if (sub == LL_IMM_TRUE) {
            vs[-1] = ll_false();
        } else if (sub == LL_IMM_FALSE) {
            vs[-1] = ll_true();
        } else {
            goto type_error;
        }
    }
    LL_NEXT(ip, env, vs, pool, rs);
type_error:
    {
        static const char msg[] = "ll: '!' operand is not a boolean, aborting\n";
        ll_fatal(msg, sizeof(msg) - 1);
    }
}
