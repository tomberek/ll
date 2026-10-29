/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445's src/asm/ops/op_if.S -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "ops.h"

LL_OP_ATTR Value ll_op_if(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    Value cond = vs[-1];
    vs--;
    if ((cond.raw & LL_TAG_MASK) != LL_TAG_IMM) goto type_error;
    {
        uint64_t sub = cond.raw >> LL_TAG_BITS;
        if (sub == LL_IMM_TRUE) {
            ip++;
        } else if (sub == LL_IMM_FALSE) {
            ip = (IrOp *)((char *)ip + ip->a);
        } else {
            goto type_error;
        }
    }
    LL_GOTO(ip, env, vs, pool, rs);
type_error:
    {
        static const char msg[] = "ll: `if' condition is not a boolean, aborting\n";
        ll_fatal(msg, sizeof(msg) - 1);
    }
}

LL_OP_ATTR Value ll_op_jmp(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    ip = (IrOp *)((char *)ip + ip->a);
    LL_GOTO(ip, env, vs, pool, rs);
}
