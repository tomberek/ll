/* M10: asnix_eval's AOT entry point -- replaces jit_driver.c's
 * per-invocation JIT-compile-and-run implementation. Mirrors
 * asnix's own dispatch.S prologue/epilogue: allocate/reuse this
 * depth's VALSTACK+RETSP buffers (asnix_eval_scratch_*, already
 * vendored in force_scratch.c), save/restore the enclosing context's
 * limits (so a nested asnix_eval, e.g. import, doesn't clobber an
 * outer invocation's bounds checks), and make exactly one real C
 * call into the program's first op handler -- every op after that
 * tail-calls the next one (op_dispatch.h's LL_NEXT) until OP_HALT
 * does a plain `return`, which unwinds straight back to the one real
 * call below. */
#include "op_dispatch.h"
#include "asnix/force_scratch.h"

int64_t asnix_eval_depth = 0;

Value asnix_eval(IrProgram *prog, Env *root_env) {
    void *saved_vlim = asnix_valstack_limit;
    void *saved_rlim = asnix_retsp_limit;

    Value *vs = (Value *)asnix_eval_scratch_valstack(asnix_eval_depth);
    RetFrame *rs = (RetFrame *)asnix_eval_scratch_retstack(asnix_eval_depth);
    asnix_valstack_limit = (char *)vs + ASNIX_EVAL_VALSTACK_BYTES;
    asnix_retsp_limit = (char *)rs + ASNIX_EVAL_RETSTACK_BYTES;
    asnix_eval_depth++;

    IrOp *ip0 = prog->ops;
    Value result = ((LLOpFn)ip0->handler)(ip0, root_env, vs, prog->pool, rs);

    asnix_eval_depth--;
    asnix_retsp_limit = saved_rlim;
    asnix_valstack_limit = saved_vlim;
    return result;
}
