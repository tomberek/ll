/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445's src/asm/ops/op_mk_list.S -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "ops.h"
#include "builtins_shared.h"

LL_OP_ATTR Value ll_op_mk_list(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    int64_t count = ip->a;
    Value *elems = vs - count; /* the popped elements already sit contiguously here */
    Value result = asnix_bshared_make_list(elems, count);
    vs = elems;
    ll_check_valstack(vs);
    vs[0] = result;
    vs++;
    LL_NEXT(ip, env, vs, pool, rs);
}
