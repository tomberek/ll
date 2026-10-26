/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 src/asm/ops/op_update.S -- see soft-wishing-engelbart.md plan */
#include "ops.h"
#include "attrs_update.h"
#include "value_util.h"

LL_OP_ATTR Value ll_op_update(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    Value rhs = vs[-1], lhs = vs[-2];
    if ((lhs.raw & ASNIX_TAG_MASK) != ASNIX_TAG_ATTRS || (rhs.raw & ASNIX_TAG_MASK) != ASNIX_TAG_ATTRS) {
        static const char msg[] = "asnix: '//' operands must both be attrsets, aborting\n";
        asnix_fatal(msg, sizeof(msg) - 1);
    }
    Value result = asnix_attrs_update(lhs, rhs);
    vs--;
    vs[-1] = result;
    LL_NEXT(ip, env, vs, pool, rs);
}

/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 src/asm/ops/op_concat.S -- see soft-wishing-engelbart.md plan */
LL_OP_ATTR Value ll_op_concat(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    Value rhs = vs[-1], lhs = vs[-2];
    /* No type-check here, matching op_concat.S: asnix_concat_lists does its
     * own TAG_LIST check on both operands and asnix_fatal()s (never
     * returns) if either isn't a list. */
    Value result = asnix_concat_lists(lhs, rhs);
    vs--;
    vs[-1] = result;
    LL_NEXT(ip, env, vs, pool, rs);
}
