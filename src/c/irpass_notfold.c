/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "asnix/irpass.h"

#include <stdint.h>

/* Folds a boolean literal immediately followed by OP_NOT into the
 * complementary boolean literal, dropping the NOT (2 ops -> 1):
 *
 *   OP_PUSH_TRUE ; OP_NOT  ->  OP_PUSH_FALSE
 *   OP_PUSH_FALSE; OP_NOT  ->  OP_PUSH_TRUE
 *
 * SAFETY, and why this is NOT the same as the general "NOT;NOT elision
 * on an arbitrary value" case a prior session explicitly investigated
 * and declined to implement: OP_NOT's own precondition (ir.h) is
 * "top of stack must be TAG_IMM true/false, else fatal" -- i.e. OP_NOT
 * is a genuine runtime type check on whatever value feeds it, and a
 * general fold that elides a NOT without knowing the ACTUAL runtime type
 * of its operand would silently skip that check, changing a program that
 * should abort with a type error into one that doesn't (or, more subtly,
 * changing a program that has a REAL side-effecting operand -- e.g. the
 * operand is itself an expression that forces a thunk that throws --
 * into one that never forces it at all). THIS pass has no such problem:
 * its only two trigger ops are OP_PUSH_TRUE and OP_PUSH_FALSE
 * themselves, which by construction ALWAYS push a genuine TAG_IMM
 * true/false (there is no way for compiler.c, or any other op, or any
 * other pass in this pipeline, to emit a PUSH_TRUE/PUSH_FALSE for
 * anything other than an actual boolean -- see ir.h's own comment on
 * both ops). So folding `PUSH_TRUE;NOT` straight to `PUSH_FALSE` (and the
 * mirror case) never bypasses OP_NOT's runtime type check on some OTHER,
 * unknown operand: the two ops being folded ARE the entire "operand
 * computation" here, and their value is already known at compile time to
 * be exactly what OP_NOT's own precondition requires. This pass does NOT
 * attempt the general "NOT;NOT" case for that reason -- it is scoped
 * exclusively to these two boolean-literal forms.
 *
 * ir.h's OP_* enum has exactly one NOT op (OP_NOT) and no separate
 * boolean and/or ops at all -- `&&`/`||` are compiled via branch-shaped
 * OP_IF/OP_JMP (see ir.h's own comment just above OP_EQ, "branch-shaped
 * OP_IF/OP_JMP (see compiler.c's AST_AND/AST_OR cases)"), not a dedicated
 * opcode -- so there is no other NOT-adjacent pattern in this IR for this
 * pass to fold beyond these two cases.
 *
 * Mirrors cmpnot.c's own hazard exactly (same shared-join-point class
 * cmpnot.c's own comment documents for OP_NOT at length): the OP_NOT at
 * i+1 is not always reached ONLY by falling through from ops[i] -- some
 * OTHER branch could jump directly to this exact OP_NOT (e.g. a
 * `(if cond then true else someOtherBoolExpr) `-shaped join point feeding
 * a single trailing `!`). Folding here rewrites ops[i] in place and
 * removes ops[i+1] (the NOT); if some other live jump targets ops[i+1]
 * directly, skipping ops[i] entirely, that jump would now land on the
 * removed op -- asnix_irpass_remove's own safety check would catch this
 * but treats it as FATAL (exit 1) rather than "skip this fold", so this
 * pass pre-checks (same incoming-jump-target guard irpass_cmpnot.c/
 * irpass_jmpnop.c/irpass_dce_if.c each independently establish for this
 * exact hazard class, duplicated here rather than shared, to keep this
 * pass self-contained in its own file per this framework's convention)
 * and simply skips (continue) rather than ever reaching that fatal exit.
 */
static int op_is_jump(int op_id) {
    return op_id == OP_IF || op_id == OP_JMP || op_id == OP_WITH_TRY || op_id == OP_SELECT_OR ||
           op_id == OP_SELECT_DYNAMIC_OR || op_id == OP_EQ_IF || op_id == OP_NEQ_IF || op_id == OP_LT_IF ||
           op_id == OP_GE_IF || op_id == OP_HAS_ATTR_IF; /* M162 */
}

static int is_jump_target(IrOp *ops, size_t len, size_t index) {
    for (size_t i = 0; i < len; i++) {
        int id = (int)(intptr_t)ops[i].handler;
        if (!op_is_jump(id)) continue;
        int64_t target = (int64_t)i + ops[i].a / ASNIX_IROP_SIZE;
        if (target == (int64_t)index) return 1;
    }
    return 0;
}

static int fold_bool_not(IrOp *ops, size_t *len, int64_t **pool, size_t *pool_len, size_t *pool_cap) {
    (void)pool;
    (void)pool_len;
    (void)pool_cap; /* strconcat-fold extension widened AsnixIrPass's signature for every pass
                      * uniformly (see irpass.h) -- this pass only folds PUSH_TRUE/FALSE;NOT
                      * pairs into a single PUSH_FALSE/TRUE, never touches the constant pool. */
    for (size_t i = 0; i + 1 < *len; i++) {
        int id = (int)(intptr_t)ops[i].handler;
        int folded;
        switch (id) {
            case OP_PUSH_TRUE: folded = OP_PUSH_FALSE; break;
            case OP_PUSH_FALSE: folded = OP_PUSH_TRUE; break;
            default: continue;
        }
        if ((int)(intptr_t)ops[i + 1].handler != OP_NOT) continue;
        if (is_jump_target(ops, *len, i + 1)) continue; /* NOT is a shared join point -- don't fold */
        ops[i].handler = (void *)(intptr_t)folded;
        ops[i].a = 0;
        ops[i].b = 0;
        asnix_irpass_remove(ops, len, i + 1, 1);
        return 1; /* re-scan from the top next iteration -- simplest correct strategy */
    }
    return 0;
}

void asnix_irpass_register_notfold(void) {
    asnix_irpass_register("notfold", fold_bool_not);
}
