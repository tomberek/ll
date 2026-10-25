/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "asnix/irpass.h"

#include <stdint.h>

/* Generalized jump-threading: when ANY jump-bearing op (OP_IF/OP_JMP/
 * OP_WITH_TRY/OP_SELECT_OR/OP_SELECT_DYNAMIC_OR -- op_is_jump's own
 * exhaustive list, same as every other pass in this framework) has a
 * target that is ITSELF an unconditional OP_JMP, redirect the FIRST
 * jump's own delta to point straight at the chain's final destination,
 * collapsing the jump-to-a-jump hop(s) entirely.
 *
 * Unlike every other pass in this framework, this one never calls
 * asnix_irpass_remove at all -- it only REWRITES an operand (ops[i].a,
 * the byte delta) on the jump-bearing op itself. That's always safe with
 * no incoming-jump guard needed: ops[i]'s own index and every OTHER op's
 * jump target are completely untouched, so nothing else in the array
 * that points AT ops[i] (or anywhere else) is affected -- the only thing
 * that changes is where ops[i] itself, when taken, lands. Contrast with
 * cfold/dce_if/cmpnot/strconcat/dce/jmpnop, which all shrink the array
 * via asnix_irpass_remove and therefore all need range_has_incoming_jump/
 * is_targeted_by_another_jump-style pre-checks (removal invalidates
 * indices and is fatal on a live jump into the removed range; a pure
 * delta rewrite has no such hazard at all).
 *
 * This is exactly the shape jmpnop.c's own header comment anticipates
 * ("a nested if inside an outer if's then-branch ... could in principle
 * after a future pass shrinks the else-branch" create a jump whose
 * target is itself just a jump) -- dce_if collapsing a dead branch, or
 * jmpnop/dce removing ops, routinely leaves what used to be a two-step
 * jump (through an intermediate OP_JMP that used to do real work, or
 * that used to be followed by now-removed code) as a jump directly to
 * another jump. This pass is the general cleanup for that whole class,
 * for every jump-bearing op, not just OP_JMP->OP_JMP.
 *
 * CHAIN FOLLOWING: resolves the WHOLE chain in one call (follows
 * target -> target's own target -> ... until the final op is no longer
 * an OP_JMP), rather than collapsing just one hop and relying on the
 * fixpoint driver to re-run for the next hop. Both strategies are
 * correct (the fixpoint driver, bounded by ASNIX_IRPASS_MAX_ITERS, would
 * eventually get there one hop at a time too); whole-chain-in-one-call
 * just avoids burning extra fixpoint iterations on a long chain, and
 * real compiled code here never produces a chain more than a couple of
 * hops deep anyway.
 *
 * CYCLE GUARD: real code this compiler emits only ever contains forward
 * jumps (see irpass_dce.c's own header comment for the exhaustive
 * argument: every delta comes from ib_patch_delta with target_idx ==
 * ib_len(b) at patch time, always > the jump's own site), so a cycle of
 * OP_JMPs pointing at each other can never actually arise from this
 * compiler, with or without every other pass in this pipeline (none of
 * cfold/dce_if/cmpnot/strconcat/dce/jmpnop ever rewrites a jump's target
 * to point BACKWARD either). Still, this pass walks the chain with a
 * hop count bounded by *len+1 (no chain of genuinely-distinct ops can be
 * longer than the array itself), and simply gives up on that one op
 * (leaves it untouched, moves on) rather than looping forever if that
 * invariant is ever violated by some future change -- defensive only,
 * never expected to trigger. */
static int op_is_jump(int op_id) {
    return op_id == OP_IF || op_id == OP_JMP || op_id == OP_WITH_TRY || op_id == OP_SELECT_OR ||
           op_id == OP_SELECT_DYNAMIC_OR || op_id == OP_EQ_IF || op_id == OP_NEQ_IF || op_id == OP_LT_IF ||
           op_id == OP_GE_IF || op_id == OP_HAS_ATTR_IF; /* M162 */
}

static int thread_jumps(IrOp *ops, size_t *len, int64_t **pool, size_t *pool_len, size_t *pool_cap) {
    (void)pool;
    (void)pool_len;
    (void)pool_cap; /* M118 strconcat-fold extension widened AsnixIrPass's signature for every
                      * pass uniformly (see irpass.h) -- this pass only rewrites a jump delta,
                      * never touches the constant pool. */
    size_t n = *len;
    size_t hop_limit = n + 1;
    for (size_t i = 0; i < n; i++) {
        int id = (int)(intptr_t)ops[i].handler;
        if (!op_is_jump(id)) continue;

        int64_t target = (int64_t)i + ops[i].a / ASNIX_IROP_SIZE;
        if (target < 0 || (size_t)target >= n) continue; /* malformed/out-of-range -- never emitted, skip defensively */
        if ((int)(intptr_t)ops[(size_t)target].handler != OP_JMP) continue;

        int64_t cur = target;
        size_t hops = 0;
        while ((int)(intptr_t)ops[(size_t)cur].handler == OP_JMP && hops < hop_limit) {
            int64_t next = cur + ops[(size_t)cur].a / ASNIX_IROP_SIZE;
            if (next == cur) break; /* a JMP targeting itself -- degenerate, stop here */
            if (next < 0 || (size_t)next >= n) break; /* malformed/out-of-range -- stop, don't chase off the end */
            cur = next;
            hops++;
        }
        if (hops >= hop_limit) continue; /* cycle guard tripped -- give up on this op, defensive only */
        if (cur == target) continue; /* chain didn't actually move anywhere -- nothing to collapse */

        ops[i].a = (cur - (int64_t)i) * ASNIX_IROP_SIZE;
        return 1; /* re-scan from the top next iteration -- simplest correct strategy */
    }
    return 0;
}

void asnix_irpass_register_jmpthread(void) {
    asnix_irpass_register("jmpthread", thread_jumps);
}
