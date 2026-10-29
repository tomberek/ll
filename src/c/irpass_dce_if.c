/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "ll/irpass.h"

#include <stdint.h>

/* Dead-branch elimination for a statically-known OP_IF condition.
 *
 * compiler.c's AST_IF codegen shape is:
 *   [cond ops]
 *   OP_IF delta=<else_start>   (pops cond; if FALSE, jump to else_start; else fall through)
 *   [then ops]
 *   OP_JMP delta=<join>        (unconditional, skips the else branch)
 * else_start:
 *   [else ops]
 * join:
 *   ...
 *
 * When the op immediately before an OP_IF is OP_PUSH_TRUE or OP_PUSH_FALSE
 * (straight from source, e.g. `if true then A else B`, or exposed by a
 * prior pass -- e.g. cfold folding a constant comparison into
 * PUSH_TRUE/PUSH_FALSE right before the IF), the branch not taken is dead
 * and the PUSH_TRUE/FALSE + IF pair is dead too: its only job was
 * deciding which way to branch, and that's now known at compile time.
 *
 * - TRUE: removing just the `PUSH_TRUE; OP_IF` pair (2 ops) is enough --
 *   execution falls straight through into the "then" branch, which
 *   already ends in its own OP_JMP to `join`, correctly skipping the
 *   (now unreachable, but still physically present) else branch.
 * - FALSE: we want execution to fall straight into the "else" branch
 *   instead, so we remove the WHOLE range [push_false_index, else_start)
 *   -- the PUSH_FALSE, the OP_IF, the entire "then" branch, and its
 *   trailing OP_JMP -- as one contiguous ll_irpass_remove call, where
 *   else_start is exactly OP_IF's own resolved target.
 *
 * Both sub-cases must check, BEFORE calling ll_irpass_remove, that no
 * jump anywhere in the whole ops array targets an index strictly inside
 * the range about to be removed -- ll_irpass_remove enforces this
 * itself, but it treats a violation as FATAL (exit 1), not a "can't fold
 * this one, try something else" signal. A real, constructible case where
 * this matters: `if A then B else (if true then X else Y)` -- the outer
 * OP_IF's else-target lands exactly on the inner PUSH_TRUE, which is
 * exactly index `start` of the inner pair's own removal range. That's
 * actually fine semantically (the outer jump should end up landing on the
 * inner "then" branch, which is exactly what the surviving op at that
 * position becomes after the fold), but ll_irpass_remove's blanket
 * check doesn't special-case "targets exactly `start`" as safe -- so we
 * pre-check ourselves and just skip (return 0, let the scan continue)
 * rather than ever reaching that fatal exit.
 */

static int op_is_jump(int op_id) {
    return op_id == OP_IF || op_id == OP_JMP || op_id == OP_WITH_TRY || op_id == OP_SELECT_OR ||
           op_id == OP_SELECT_DYNAMIC_OR || op_id == OP_EQ_IF || op_id == OP_NEQ_IF || op_id == OP_LT_IF ||
           op_id == OP_GE_IF || op_id == OP_HAS_ATTR_IF; /* M162 */
}

/* Mirrors ll_irpass_remove's own internal safety scan (irpass.c) so we
 * can decide NOT to attempt a removal that primitive would otherwise abort
 * the whole process over. Returns 1 if some jump (anywhere in ops[0..len))
 * targets an index inside [start, start+count). */
static int range_has_incoming_jump(IrOp *ops, size_t len, size_t start, size_t count) {
    for (size_t i = 0; i < len; i++) {
        int id = (int)(intptr_t)ops[i].handler;
        if (!op_is_jump(id)) continue;
        int64_t target = (int64_t)i + ops[i].a / LL_IROP_SIZE;
        if (target >= (int64_t)start && target < (int64_t)(start + count)) return 1;
    }
    return 0;
}

static int dce_if(IrOp *ops, size_t *len, int64_t **pool, size_t *pool_len, size_t *pool_cap) {
    (void)pool;
    (void)pool_len;
    (void)pool_cap; /* M118 strconcat-fold extension widened LlIrPass's signature for every
                      * pass uniformly (see irpass.h) -- this pass only removes dead-branch ops,
                      * never touches the constant pool. */
    size_t n = *len;
    for (size_t i = 0; i + 1 < n; i++) {
        int id = (int)(intptr_t)ops[i].handler;
        if (id != OP_PUSH_TRUE && id != OP_PUSH_FALSE) continue;
        if ((int)(intptr_t)ops[i + 1].handler != OP_IF) continue;

        if (id == OP_PUSH_TRUE) {
            /* Remove just the [PUSH_TRUE, OP_IF] pair -- execution then
             * falls straight through into the existing "then" branch. */
            if (range_has_incoming_jump(ops, n, i, 2)) continue;
            ll_irpass_remove(ops, len, i, 2);
            return 1;
        }

        /* OP_PUSH_FALSE: resolve OP_IF's own else-target (self-relative to
         * the IF op's own index, i + 1) and remove
         * [i, else_start) -- PUSH_FALSE, OP_IF, the whole "then" branch,
         * and its trailing OP_JMP -- all in one shot, so execution falls
         * straight into what's left: the "else" branch. */
        int64_t else_start = (int64_t)(i + 1) + ops[i + 1].a / LL_IROP_SIZE;
        if (else_start <= (int64_t)(i + 1)) continue; /* malformed/degenerate -- never emitted by compiler.c, skip defensively */
        size_t count = (size_t)else_start - i;
        if (range_has_incoming_jump(ops, n, i, count)) continue;
        ll_irpass_remove(ops, len, i, count);
        return 1;
    }
    return 0;
}

void ll_irpass_register_dce_if(void) {
    ll_irpass_register("dce_if", dce_if);
}
