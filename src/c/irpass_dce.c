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
 * Today's compiler never actually emits any dead op, and every jump this
 * compiler emits points strictly FORWARD (target index > source index),
 * never backward -- this pass still finds 0 removal opportunities on any
 * currently-compiled program as a result. It's a safety net for whatever
 * FUTURE pass first creates a genuinely dead region (e.g. a dead-branch
 * eliminator that notices an OP_IF's condition is a compile-time constant
 * and wants to drop the untaken side), not a transform expected to fire
 * today.
 *
 * REMOVAL ORDERING -- the one real subtlety here: ll_irpass_remove's own
 * safety check (irpass.c) scans EVERY op currently in the array (dead or
 * live) and is fatal if ANY of them has a jump target landing inside
 * [start, start+count) -- including an op that is ITSELF inside that
 * very range. That means removing a whole maximal contiguous dead range
 * in ONE call is only safe if NOTHING inside that range jumps to
 * anything else inside it -- which is false in general (a dead `if a
 * then b else c` sub-expression has an OP_IF/OP_JMP pair whose target is
 * a few ops further along, fully inside the same dead range): removing a
 * 3-op range containing a single internal OP_JMP pointing at its own
 * last op is fatal, even though the whole range is unreachable and the
 * jump's source is being removed in the very same call.
 *
 * The fix: remove dead ops ONE AT A TIME, in ASCENDING order of index
 * (lowest dead index first), not "maximal ranges, descending". This is
 * safe by a short inductive argument that only relies on the
 * forward-jump invariant established above:
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
           op_id == OP_GE_IF || op_id == OP_HAS_ATTR_IF;
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
        if (!is_terminator && !is_unconditional_jump) {
            size_t nxt = i + 1;
            if (nxt < n && !reachable[nxt]) {
                reachable[nxt] = 1;
                stack[sp++] = nxt;
            }
        }
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
    (void)pool_cap;
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
        /* orig_i's current position is orig_i - removed_so_far (see
         * REMOVAL ORDERING in the file header comment). */
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
