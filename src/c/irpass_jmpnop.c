/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "ll/irpass.h"

#include <stdint.h>

/* Removes a redundant OP_JMP whose target is exactly the op right after
 * it -- i.e. a jump that's a no-op: falling through (not jumping at all)
 * lands in exactly the same place. The no-op case is simply
 * `a == LL_IROP_SIZE`, i.e. target_index == i + 1.
 *
 * This never legitimately happens from compiler.c's own direct emission,
 * but it CAN appear as a side effect of an earlier pass's own rewrite --
 * e.g. dce_if-style dead-branch elimination collapsing what used to be a
 * real jump target into the immediately-following op. Dropping it is a
 * pure control-flow simplification: no operand is popped/pushed by
 * OP_JMP, so removing it changes nothing observable other than one fewer
 * dispatch per execution that reaches this point.
 *
 * Hazard: a DIFFERENT live jump can land exactly ON this no-op OP_JMP's
 * own index rather than skipping past it -- e.g. a nested `if` inside an
 * outer `if`'s then-branch, where the inner if's trailing OP_JMP targets
 * the outer if's own trailing OP_JMP.
 * Removing it would actually still be semantically safe (jumping to a
 * no-op is equivalent to falling through it to whatever IT falls through
 * to), but ll_irpass_remove's own contract is unconditional: ANY live
 * jump landing inside [i, i+1) -- including exactly AT i -- is fatal,
 * not just one that would land strictly past it, and the generic shrink
 * primitive can't know that. So: only fire when no OTHER jump-bearing op
 * targets this exact index. */
static int op_is_jump(int id) {
    return id == OP_IF || id == OP_JMP || id == OP_WITH_TRY || id == OP_SELECT_OR ||
           id == OP_SELECT_DYNAMIC_OR || id == OP_EQ_IF || id == OP_NEQ_IF || id == OP_LT_IF ||
           id == OP_GE_IF || id == OP_HAS_ATTR_IF;
}

static int is_targeted_by_another_jump(IrOp *ops, size_t len, size_t idx) {
    for (size_t j = 0; j < len; j++) {
        if (j == idx) continue;
        int id = (int)(intptr_t)ops[j].handler;
        if (!op_is_jump(id)) continue;
        int64_t target = (int64_t)j + ops[j].a / (int64_t)LL_IROP_SIZE;
        if (target == (int64_t)idx) return 1;
    }
    return 0;
}

static int remove_noop_jmp(IrOp *ops, size_t *len, int64_t **pool, size_t *pool_len, size_t *pool_cap) {
    (void)pool;
    (void)pool_len;
    (void)pool_cap;
    for (size_t i = 0; i < *len; i++) {
        if ((int)(intptr_t)ops[i].handler != OP_JMP) continue;
        if (ops[i].a != (int64_t)LL_IROP_SIZE) continue;
        if (is_targeted_by_another_jump(ops, *len, i)) continue;
        ll_irpass_remove(ops, len, i, 1);
        return 1;
    }
    return 0;
}

void ll_irpass_register_jmpnop(void) {
    ll_irpass_register("jmpnop", remove_noop_jmp);
}
