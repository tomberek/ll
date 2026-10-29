/* M10: ll_eval's AOT entry point -- replaces jit_driver.c's
 * per-invocation JIT-compile-and-run implementation. Mirrors
 * asnix's own dispatch.S prologue/epilogue: allocate/reuse this
 * depth's VALSTACK+RETSP buffers (ll_eval_scratch_*, already
 * vendored in force_scratch.c), save/restore the enclosing context's
 * limits (so a nested ll_eval, e.g. import, doesn't clobber an
 * outer invocation's bounds checks), and make exactly one real C
 * call into the program's first op handler -- every op after that
 * tail-calls the next one (op_dispatch.h's LL_NEXT) until OP_HALT
 * does a plain `return`, which unwinds straight back to the one real
 * call below. */
#include "op_dispatch.h"
#include "ll/force_scratch.h"

int64_t ll_eval_depth = 0;

Value ll_eval(IrProgram *prog, Env *root_env) {
    void *saved_vlim = ll_valstack_limit;
    void *saved_rlim = ll_retsp_limit;

    Value *vs = (Value *)ll_eval_scratch_valstack(ll_eval_depth);
    RetFrame *rs = (RetFrame *)ll_eval_scratch_retstack(ll_eval_depth);
    ll_valstack_limit = (char *)vs + LL_EVAL_VALSTACK_BYTES;
    ll_retsp_limit = (char *)rs + LL_EVAL_RETSTACK_BYTES;
    ll_eval_depth++;

    IrOp *ip0 = prog->ops;
    Value result = ((LLOpFn)ip0->handler)(ip0, root_env, vs, prog->pool, rs);

    ll_eval_depth--;
    ll_retsp_limit = saved_rlim;
    ll_valstack_limit = saved_vlim;
    return result;
}
