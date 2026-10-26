/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445's src/asm/ops/op_halt.S -- see ~/.claude/plans/soft-wishing-engelbart.md
 *
 * The one genuine `return`: because every call that reached here was
 * a guaranteed tail call (musttail), at most one native stack frame
 * is live below it, so this unwinds straight back to the single real
 * entry call (ll_eval.c's asnix_eval, or closure_helpers.c's
 * asnix_force_for_print/asnix_apply re-entry epilogues). Those
 * callers own the depth-counter/limit bookkeeping asnix's own
 * asnix_op_halt does inline in asm -- no need to duplicate it here. */
#include "ops.h"

LL_OP_ATTR Value ll_op_halt(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    (void)ip;
    (void)env;
    (void)pool;
    (void)rs;
    return vs[-1];
}
