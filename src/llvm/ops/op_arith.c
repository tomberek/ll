/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445's src/asm/ops/op_arith.S -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "ops.h"
#include "value_util.h"

LL_OP_ATTR Value ll_op_add(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    Value rhs = vs[-1], lhs = vs[-2];
    Value result;
    if ((lhs.raw & LL_TAG_MASK) == LL_TAG_INT && (rhs.raw & LL_TAG_MASK) == LL_TAG_INT) {
        int64_t l = (int64_t)lhs.raw >> LL_TAG_BITS;
        int64_t r = (int64_t)rhs.raw >> LL_TAG_BITS;
        int64_t sum = l + r;
        if (sum > LL_INT_MAX || sum < LL_INT_MIN) {
            static const char msg[] = "ll: integer overflow in '+', aborting\n";
            ll_fatal(msg, sizeof(msg) - 1);
        }
        result.raw = ((uint64_t)sum << LL_TAG_BITS) | LL_TAG_INT;
    } else {
        result = ll_add_values(lhs, rhs);
    }
    vs--;
    vs[-1] = result;
    LL_NEXT(ip, env, vs, pool, rs);
}

LL_OP_ATTR Value ll_op_sub(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    Value rhs = vs[-1], lhs = vs[-2];
    Value result;
    if ((lhs.raw & LL_TAG_MASK) == LL_TAG_INT && (rhs.raw & LL_TAG_MASK) == LL_TAG_INT) {
        int64_t l = (int64_t)lhs.raw >> LL_TAG_BITS;
        int64_t r = (int64_t)rhs.raw >> LL_TAG_BITS;
        int64_t diff = l - r;
        if (diff > LL_INT_MAX || diff < LL_INT_MIN) {
            static const char msg[] = "ll: integer overflow in '-', aborting\n";
            ll_fatal(msg, sizeof(msg) - 1);
        }
        result.raw = ((uint64_t)diff << LL_TAG_BITS) | LL_TAG_INT;
    } else {
        result = ll_sub_values(lhs, rhs);
    }
    vs--;
    vs[-1] = result;
    LL_NEXT(ip, env, vs, pool, rs);
}

LL_OP_ATTR Value ll_op_mul(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    Value rhs = vs[-1], lhs = vs[-2];
    Value result;
    if ((lhs.raw & LL_TAG_MASK) == LL_TAG_INT && (rhs.raw & LL_TAG_MASK) == LL_TAG_INT) {
        int64_t l = (int64_t)lhs.raw >> LL_TAG_BITS;
        int64_t r = (int64_t)rhs.raw >> LL_TAG_BITS;
        int64_t prod;
        if (__builtin_mul_overflow(l, r, &prod) || prod > LL_INT_MAX || prod < LL_INT_MIN) {
            static const char msg[] = "ll: integer overflow in '*', aborting\n";
            ll_fatal(msg, sizeof(msg) - 1);
        }
        result.raw = ((uint64_t)prod << LL_TAG_BITS) | LL_TAG_INT;
    } else {
        result = ll_mul_values(lhs, rhs);
    }
    vs--;
    vs[-1] = result;
    LL_NEXT(ip, env, vs, pool, rs);
}

LL_OP_ATTR Value ll_op_div(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    Value rhs = vs[-1], lhs = vs[-2];
    Value result;
    if ((lhs.raw & LL_TAG_MASK) == LL_TAG_INT && (rhs.raw & LL_TAG_MASK) == LL_TAG_INT) {
        int64_t l = (int64_t)lhs.raw >> LL_TAG_BITS;
        int64_t r = (int64_t)rhs.raw >> LL_TAG_BITS;
        if (r == 0) {
            static const char msg[] = "ll: division by zero, aborting\n";
            ll_fatal(msg, sizeof(msg) - 1);
        }
        int64_t q = l / r; /* C truncates toward zero, matching idivq */
        if (q > LL_INT_MAX || q < LL_INT_MIN) {
            static const char msg[] = "ll: integer overflow in '/', aborting\n";
            ll_fatal(msg, sizeof(msg) - 1);
        }
        result.raw = ((uint64_t)q << LL_TAG_BITS) | LL_TAG_INT;
    } else {
        result = ll_div_values(lhs, rhs);
    }
    vs--;
    vs[-1] = result;
    LL_NEXT(ip, env, vs, pool, rs);
}
