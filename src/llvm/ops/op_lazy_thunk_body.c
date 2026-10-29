/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445's src/asm/ops/op_lazy_thunk_body.S -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "ops.h"

#include "compiler.h"

/* b = pool index of a LazyThunkDescriptor* (compiler.c). The ONLY op in a
 * stub IrProgram -- entered exactly like any other thunk body (same
 * dispatch, same already-pushed RetFrame, via op_var_ref.c's own
 * ip=body_prog->ops/pool=body_prog->pool/env=thunk_env musttail jump). */
LL_OP_ATTR Value ll_op_lazy_thunk_body(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    int64_t pidx = ip->b;
    void *desc = (void *)(uintptr_t)pool[pidx];

    IrProgram *real = ll_compile_lazy_thunk_body(desc);

    pool = real->pool;
    ip = real->ops;
    /* Fall into the real first op in the SAME dispatch context: no
     * re-entrant force/apply call, no new RetFrame. LL_GOTO is a
     * musttail jump, not a call, which preserves that. */
    LL_GOTO(ip, env, vs, pool, rs);
}
