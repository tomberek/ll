/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "asnix/irpass.h"

#include <stdint.h>

/* Removes a redundant OP_JMP whose target is exactly the op right after
 * it -- i.e. a jump that's a no-op: falling through (not jumping at all)
 * lands in exactly the same place. ib_patch_delta (irbuild.c) sets an
 * OP_JMP's field `a` to `(target_idx - site) * ASNIX_IROP_SIZE`, and
 * asnix_irpass_remove/every jump consumer recovers the target the same
 * way: `target_index = i + a/ASNIX_IROP_SIZE` (see ir.h's own comment on
 * the jump-delta-bearing ops, irpass.h's op_is_jump). So the no-op case
 * is simply `a == ASNIX_IROP_SIZE`, i.e. target_index == i + 1 -- no
 * division needed, `a` is always an exact multiple of ASNIX_IROP_SIZE
 * (every delta this project ever writes comes from ib_patch_delta).
 *
 * This never legitimately happens from compiler.c's own direct emission
 * (an OP_JMP whose only job is to skip to the very next op has no
 * purpose and compiler.c never emits one on purpose), but it CAN appear
 * as a side effect of an earlier pass's own rewrite -- e.g. dce_if-style
 * dead-branch elimination collapsing what used to be a real jump target
 * into the immediately-following op. Dropping it is a pure control-flow
 * simplification: no operand is popped/pushed by OP_JMP (see op_jmp.S),
 * so removing it changes nothing observable other than one fewer
 * dispatch per execution that reaches this point.
 *
 * Hazard found while testing this pass in isolation (see
 * irpass_jmpnop's own scratch coverage): a DIFFERENT live jump can land
 * exactly ON this no-op OP_JMP's own index rather than skipping past it
 * -- e.g. a nested `if` inside an outer `if`'s then-branch: the inner
 * if's own trailing OP_JMP (to "the end of the inner if") targets
 * whatever op compiler.c emits right after it, which for
 * `if a then (if b then x else y) else z` is the OUTER if's own trailing
 * OP_JMP (to "the end of the whole if"). That outer OP_JMP is only ever
 * a *candidate* for this pass if ITS OWN target is also just the next
 * op (which doesn't happen from compiler.c's direct emission alone, but
 * could in principle after a future pass shrinks the else-branch), so
 * this can't currently arise from this fork's only other pass (cfold,
 * which never touches jumps) -- but asnix_irpass_remove's own contract
 * is unconditional: ANY live jump landing inside [i, i+1) -- including
 * exactly AT i -- is fatal, not just one that would land strictly past
 * it. Removing it would actually still be semantically safe (jumping to
 * a no-op is equivalent to falling through it to whatever IT falls
 * through to), but the generic shrink primitive can't know that and
 * aborts the whole process rather than silently miscompiling. So: only
 * fire when no OTHER jump-bearing op targets this exact index, leaving
 * such a (currently unreachable, but cheap to guard) configuration
 * alone rather than risking a hard abort. */
static int op_is_jump(int id) {
    return id == OP_IF || id == OP_JMP || id == OP_WITH_TRY || id == OP_SELECT_OR ||
           id == OP_SELECT_DYNAMIC_OR || id == OP_EQ_IF || id == OP_NEQ_IF || id == OP_LT_IF ||
           id == OP_GE_IF || id == OP_HAS_ATTR_IF; /* M162 */
}

static int is_targeted_by_another_jump(IrOp *ops, size_t len, size_t idx) {
    for (size_t j = 0; j < len; j++) {
        if (j == idx) continue;
        int id = (int)(intptr_t)ops[j].handler;
        if (!op_is_jump(id)) continue;
        int64_t target = (int64_t)j + ops[j].a / (int64_t)ASNIX_IROP_SIZE;
        if (target == (int64_t)idx) return 1;
    }
    return 0;
}

static int remove_noop_jmp(IrOp *ops, size_t *len, int64_t **pool, size_t *pool_len, size_t *pool_cap) {
    (void)pool;
    (void)pool_len;
    (void)pool_cap; /* M118 strconcat-fold extension widened AsnixIrPass's signature for every
                      * pass uniformly (see irpass.h) -- this pass only removes a no-op OP_JMP,
                      * never touches the constant pool. */
    for (size_t i = 0; i < *len; i++) {
        if ((int)(intptr_t)ops[i].handler != OP_JMP) continue;
        if (ops[i].a != (int64_t)ASNIX_IROP_SIZE) continue;
        if (is_targeted_by_another_jump(ops, *len, i)) continue;
        asnix_irpass_remove(ops, len, i, 1);
        return 1; /* re-scan from the top next iteration -- simplest correct strategy */
    }
    return 0;
}

void asnix_irpass_register_jmpnop(void) {
    asnix_irpass_register("jmpnop", remove_noop_jmp);
}
