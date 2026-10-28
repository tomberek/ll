/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445's src/asm/ops/op_lazy_thunk_body.S
 * (lazy-thunk-compile experiment, docs/lazy-thunk-compile-proposal.md) --
 * see ~/.claude/plans/soft-wishing-engelbart.md */
#include "ops.h"

#include "compiler.h"

/* b = pool index of a LazyThunkDescriptor* (compiler.c). The ONLY op in a
 * stub IrProgram -- entered exactly like any other thunk body (same
 * dispatch, same already-pushed RetFrame, via op_var_ref.c's own
 * ip=body_prog->ops/pool=body_prog->pool/env=thunk_env musttail jump).
 *
 * asnix's own asm handler has to carefully save/restore %rdi (the real
 * captured ENV) across its `call asnix_compile_lazy_thunk_body`, because
 * the SysV calling convention doesn't do that for a leaf asm routine on
 * its own -- that's the one real bug the design doc's own first draft
 * had (clobbered %rdi before saving it). Here, in plain C, `env` is just
 * a function parameter: the C calling convention already preserves it
 * across the call to asnix_compile_lazy_thunk_body below for free, so
 * there is nothing to save/restore -- the entire hazard the asm version
 * has to work around doesn't exist in this backend. */
LL_OP_ATTR Value ll_op_lazy_thunk_body(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    int64_t pidx = ip->b;
    void *desc = (void *)(uintptr_t)pool[pidx];

    IrProgram *real = asnix_compile_lazy_thunk_body(desc); /* mutates desc->self's ops/pool
                                                              * in place, returns desc->self */

    pool = real->pool;
    ip = real->ops;
    /* Fall into the real first op, in the SAME dispatch context -- no
     * re-entrant force/apply call, no new RetFrame (the exact property
     * asnix's own M187 "Conclusion" asked for, confirmed preserved here:
     * LL_GOTO is a musttail jump, not a call). */
    LL_GOTO(ip, env, vs, pool, rs);
}
