/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "ll/irpass.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Global reachability-based dead code elimination.
 *
 * Computes the set of op indices reachable from op 0 by:
 *   (a) ordinary fallthrough i -> i+1, for every op that ISN'T OP_RET/
 *       OP_HALT (terminators -- no successor at all) and ISN'T OP_JMP
 *       (unconditional -- its ONLY successor is its own jump target, see
 *       (b) below; NOT a fallthrough too, since it's never actually
 *       taken);
 *   (b) each jump-delta-bearing op's own target: OP_JMP (target only,
 *       per (a) above), and OP_IF/OP_WITH_TRY/OP_SELECT_OR/
 *       OP_SELECT_DYNAMIC_OR (target, PLUS the ordinary i -> i+1
 *       fallthrough from (a), since these four are conditional -- either
 *       side may run depending on a runtime value this pass never
 *       inspects).
 * Any index never reached this way is PROVABLY never executed no matter
 * what runtime values flow through -- dead -- and is removed.
 *
 * Today's compiler (compiler.c/irbuild.c) never actually emits any dead
 * op: every ib_ret()/ib_halt() call is the LAST emit into its own
 * IrBuilder before that same builder's ib_finish() (grepped exhaustively
 * -- every ib_ret/ib_halt call site is immediately followed by
 * ib_finish(that same builder), nothing else is ever appended after),
 * and every OP_IF/OP_JMP/OP_WITH_TRY/OP_SELECT_OR/OP_SELECT_DYNAMIC_OR's
 * delta is patched via ib_patch_delta(b, site, target_idx) with
 * target_idx == ib_len(b) AT PATCH TIME, i.e. the position the NEXT op
 * will land at, which is always > site since nothing is ever inserted
 * before the already-emitted jump site -- so every jump this compiler
 * ever emits points strictly FORWARD (target index > source index),
 * never backward, and no other pass in this pipeline (cfold never
 * touches a jump op; this pass only removes, never adds, a jump)
 * introduces one either. This pass still finds 0 removal opportunities
 * on any currently-compiled program as a result -- it's a safety net for
 * whatever FUTURE pass first creates a genuinely dead region (e.g. a
 * dead-branch eliminator that notices an OP_IF's condition is a
 * compile-time constant and wants to drop the untaken side), not a
 * transform expected to fire today. Verified by running the full
 * LL_IR_OPT=1 vs. unset diff sweep with this pass registered anyway
 * (see the commit message / session report for the exact counts).
 *
 * REMOVAL ORDERING -- the one real subtlety here, worth a long comment:
 * ll_irpass_remove's own safety check (irpass.c) scans EVERY op
 * currently in the array (dead or live, not just ones outside the range
 * being removed) and is fatal if ANY of them has a jump target landing
 * inside [start, start+count) -- including an op that is ITSELF inside
 * that very range. That means removing a whole maximal contiguous dead
 * range in ONE call is only safe if NOTHING inside that range jumps to
 * anything else inside it -- which is false in general (a dead `if a
 * then b else c` sub-expression has an OP_IF/OP_JMP pair whose target is
 * a few ops further along, fully inside the same dead range) --
 * confirmed empirically with a standalone test against the exact
 * primitive before writing this comment: removing a 3-op range
 * containing a single internal OP_JMP pointing at its own last op is
 * fatal, even though the whole range is unreachable and the jump's
 * source is being removed in the very same call.
 *
 * The fix: remove dead ops ONE AT A TIME, in ASCENDING order of index
 * (lowest dead index first), not the "maximal ranges, descending" order
 * a first draft of this pass's own spec suggested. This is safe by a
 * short inductive argument that only relies on the forward-jump
 * invariant established above (and, independently, re-checked by
 * ll_irpass_remove itself on every single call -- this pass never
 * bypasses that check, it just arranges to never need to trip it):
 *   - every jump TO a dead index must itself come from a DEAD op: if a
 *     REACHABLE op jumped to index d, d would have been marked reachable
 *     by this same BFS, contradicting d being dead.
 *   - every jump is strictly forward (source index < target index, see
 *     above), so any dead op jumping to dead index d has its OWN index
 *     strictly less than d.
 *   - processing dead indices in ascending order therefore guarantees:
 *     by the time we remove index d, every op that could possibly jump
 *     to d (which must be dead AND have a smaller index) has ALREADY
 *     been removed in an earlier iteration of this same sweep -- so
 *     ll_irpass_remove's own scan finds no surviving jump into
 *     [d, d+1) and never trips its fatal check.
 * Each removal shifts every later index down by exactly one; since we
 * only ever remove indices drawn from our own ascending dead list, the
 * k-th dead index removed (0-based) sits at CURRENT position
 * (original_index - k) when its turn comes, which is tracked directly
 * below instead of re-deriving it from a fresh reachability pass each
 * time. */

static int dce_op_is_jump(int op_id) {
    return op_id == OP_IF || op_id == OP_JMP || op_id == OP_WITH_TRY || op_id == OP_SELECT_OR ||
           op_id == OP_SELECT_DYNAMIC_OR || op_id == OP_EQ_IF || op_id == OP_NEQ_IF || op_id == OP_LT_IF ||
           op_id == OP_GE_IF || op_id == OP_HAS_ATTR_IF; /* M162 */
}

/* BFS/worklist over ops[0..n) starting at index 0, marking `reachable`
 * (caller-owned, pre-zeroed, size n). Bounded by n (each index pushed at
 * most once, guarded by the reachable[] check before pushing). */
static void dce_mark_reachable(const IrOp *ops, size_t n, unsigned char *reachable, size_t *stack) {
    size_t sp = 0;
    if (n == 0) return;
    reachable[0] = 1;
    stack[sp++] = 0;
    while (sp > 0) {
        size_t i = stack[--sp];
        int id = (int)(intptr_t)ops[i].handler;
        int is_terminator = (id == OP_RET || id == OP_HALT);
        int is_unconditional_jump = (id == OP_JMP);
        /* (a) ordinary fallthrough -- every op except the two
         * terminators and OP_JMP (whose only successor is its own
         * target, see below; it is never actually fallen through to
         * i+1, unlike the other four jump-bearing ops). */
        if (!is_terminator && !is_unconditional_jump) {
            size_t nxt = i + 1;
            if (nxt < n && !reachable[nxt]) {
                reachable[nxt] = 1;
                stack[sp++] = nxt;
            }
        }
        /* (b) every jump-delta-bearing op's own target. */
        if (dce_op_is_jump(id)) {
            int64_t target = (int64_t)i + ops[i].a / LL_IROP_SIZE;
            if (target >= 0 && (size_t)target < n && !reachable[(size_t)target]) {
                reachable[(size_t)target] = 1;
                stack[sp++] = (size_t)target;
            }
        }
    }
}

/* Finds every dead index in one reachability sweep and removes all of
 * them before returning -- see the file header comment above for why
 * ascending, one-at-a-time removal (not the maximal-contiguous-range
 * batching this pass's own spec first suggested) is the safe approach
 * here. Returns 1 if anything was removed (the fixpoint driver will call
 * again; the next call's own fresh BFS will find nothing left to do and
 * return 0), 0 if the whole array was already fully reachable. */
static int dead_code_eliminate(IrOp *ops, size_t *len, int64_t **pool, size_t *pool_len, size_t *pool_cap) {
    (void)pool;
    (void)pool_len;
    (void)pool_cap; /* M118 strconcat-fold extension widened LlIrPass's signature for every
                      * pass uniformly (see irpass.h) -- this pass only removes unreachable ops,
                      * never touches the constant pool. */
    size_t n = *len;
    if (n == 0) return 0;

    unsigned char *reachable = calloc(n, 1);
    size_t *stack = malloc(n * sizeof(size_t));
    if (!reachable || !stack) {
        fprintf(stderr, "dce: out of memory\n");
        exit(1);
    }
    dce_mark_reachable(ops, n, reachable, stack);
    free(stack);

    int removed_any = 0;
    size_t removed_so_far = 0;
    for (size_t orig_i = 0; orig_i < n; orig_i++) {
        if (reachable[orig_i]) continue;
        /* orig_i is dead. Every op that could jump to it is also dead
         * (else orig_i would be reachable) and has a strictly smaller
         * original index (forward-jump invariant, see header comment)
         * -- so it has already been removed by an earlier loop
         * iteration (we only ever remove indices from THIS SAME
         * ascending dead list). Its current position is therefore
         * orig_i - removed_so_far. */
        ll_irpass_remove(ops, len, orig_i - removed_so_far, 1);
        removed_so_far++;
        removed_any = 1;
    }
    free(reachable);
    return removed_any;
}

void ll_irpass_register_dce(void) {
    ll_irpass_register("dce", dead_code_eliminate);
}
