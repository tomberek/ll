/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "ll/irpass.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Frame-slot constant propagation.
 *
 * compiler.c's AST_LET case (and every other FRAME_ALLOC/FRAME_SET
 * producer) routes a bare compile-time-constant binding through a real
 * frame slot: `let x = 5; in x + x + x` compiles to `PUSH_INT 5;
 * FRAME_SET 0; ...; VAR_REF depth=0 slot=0; ...`, leaving every later
 * read as a real frame-slot VAR_REF rather than the directly cheaper push
 * the same information already makes safe. This pass closes that gap: it
 * rewrites every OP_VAR_REF that is PROVABLY reading back a constant that
 * was JUST placed there, unconditionally, to the same push op instead.
 *
 * SAFETY ARGUMENT, by construction, not by assumption:
 *
 * 1. A slot `S` at frame-nesting level `L` is recorded as "constant"
 *    only when ops[i] is OP_FRAME_SET(a=S) and ops[i-1] is a bare
 *    OP_PUSH_INT/OP_PUSH_TRUE/OP_PUSH_FALSE/OP_PUSH_NULL -- the value
 *    stored is PROVABLY a fixed literal, never a thunk or computed
 *    expression.
 *
 * 2. "Frame-nesting level" tracks OP_FRAME_ALLOC/OP_WITH (increment) and
 *    OP_FRAME_EXIT/OP_WITH_EXIT (decrement) in program order -- the same
 *    convention OP_VAR_REF's own `depth` operand walks at runtime. This
 *    pass only ever rewrites `depth == 0` reads, which by that same
 *    convention ALWAYS refer to whatever frame is innermost at that exact
 *    program point; a reference reaching past this frame to an enclosing
 *    one necessarily has depth > 0 and is never touched here.
 *
 * 3. A recorded slot is invalidated (treated as no-longer-constant) the
 *    moment, at the SAME level before the read being considered, either:
 *    another OP_FRAME_SET to the same slot, or an OP_VAR_REF_CELL on the
 *    same slot (a caller materializing a raw, mutable Cell pointer at
 *    this slot -- excluded conservatively).
 *
 * 4. THE ONE REAL HAZARD: a jump from elsewhere in the program could
 *    land DIRECTLY on the OP_VAR_REF being considered, or on any op
 *    between the recording OP_FRAME_SET and it, having skipped the
 *    FRAME_SET (or an invalidating op) entirely -- the slot's tracked
 *    state at that point would then be unknown, not whatever this pass's
 *    own linear scan believes. Fixed by precomputing, once per program,
 *    which op indices are the target of ANY jump anywhere in the array,
 *    and refusing to record OR replace at any index that is itself a
 *    jump target, OR to record from a push/set pair where either op is a
 *    jump target. Since nothing can jump INTO the middle of a
 *    jump-target-free span, the only way to reach the OP_VAR_REF being
 *    replaced is to have fallen through it in program order all the way
 *    from (at the latest) the recording OP_FRAME_SET -- which, combined
 *    with (3)'s invalidation rule, is what makes the replacement sound.
 *
 * POOL ALIASING HAZARD: an earlier version of this pass reused the
 * ORIGINAL OP_PUSH_INT's own pool index directly for every replacement
 * (reasoning: pool entries are read-only once pushed, so sharing felt
 * free). That reasoning was wrong -- irpass_cfold.c's own fold overwrites
 * a pool slot IN PLACE when it folds a PUSH_INT pair (`(*pool)[ops[i].b]
 * = result`). Reusing one pool index across N replacements breaks that:
 * `let x = 5; in x + x + x` under `LL_IR_OPT=1` computed 20, not 15,
 * because cfold's first fold (5+5=10) overwrote the SHARED pool slot in
 * place, and the THIRD occurrence (still pointing at that same,
 * now-mutated, slot) then folded 10+10 instead of 10+5. Fixed by giving
 * every replacement its OWN fresh pool entry via `ll_irpass_pool_append`
 * instead of ever sharing an index with another op.
 *
 * Deliberately NOT attempted: propagating a constant across a
 * closure-capture boundary (OP_MK_THUNK/OP_LAMBDA each compile to a
 * SEPARATE IrProgram with its own, independent ll_irpass_run call) --
 * this pass only ever sees one flat ops array at a time.
 *
 * Bounded, not unbounded: only the first LL_CONSTPROP_MAX_SLOTS distinct
 * slot indices in a given program are tracked at all; a slot index at or
 * beyond that bound is silently never recorded and never replaced --
 * always SAFE (a missed optimization, never a wrong one).
 */

static int constprop_op_is_jump(int op_id) {
    return op_id == OP_IF || op_id == OP_JMP || op_id == OP_WITH_TRY || op_id == OP_SELECT_OR ||
           op_id == OP_SELECT_DYNAMIC_OR || op_id == OP_EQ_IF || op_id == OP_NEQ_IF || op_id == OP_LT_IF ||
           op_id == OP_GE_IF || op_id == OP_HAS_ATTR_IF;
}

static int constprop_is_const_push(int op_id) {
    return op_id == OP_PUSH_INT || op_id == OP_PUSH_TRUE || op_id == OP_PUSH_FALSE || op_id == OP_PUSH_NULL;
}

#define LL_CONSTPROP_MAX_SLOTS 256

typedef struct {
    int64_t level;    /* frame-nesting level this record was established at; -1 == never set/invalidated */
    int32_t kind;      /* OP_PUSH_INT/OP_PUSH_TRUE/OP_PUSH_FALSE/OP_PUSH_NULL */
    int64_t value;     /* meaningful only when kind == OP_PUSH_INT; each replacement gets its OWN
                         * fresh pool entry holding this value (see POOL ALIASING HAZARD above). */
} ConstSlot;

static int constprop_run(IrOp *ops, size_t *len, int64_t **pool, size_t *pool_len, size_t *pool_cap) {
    size_t n = *len;
    if (n == 0) return 0;

    unsigned char *is_target = calloc(n, 1);
    if (!is_target) return 0; /* allocation failure: skip this call entirely, never miscompile for it */
    for (size_t i = 0; i < n; i++) {
        int id = (int)(intptr_t)ops[i].handler;
        if (!constprop_op_is_jump(id)) continue;
        int64_t target = (int64_t)i + ops[i].a / LL_IROP_SIZE;
        if (target >= 0 && (size_t)target < n) is_target[(size_t)target] = 1;
    }

    ConstSlot slots[LL_CONSTPROP_MAX_SLOTS];
    for (size_t s = 0; s < LL_CONSTPROP_MAX_SLOTS; s++) slots[s].level = -1;

    int fired = 0;
    int64_t level = 0;

    for (size_t i = 0; i < n; i++) {
        int id = (int)(intptr_t)ops[i].handler;

        if (id == OP_FRAME_ALLOC || id == OP_WITH) {
            level++;
            continue;
        }
        if (id == OP_FRAME_EXIT || id == OP_WITH_EXIT) {
            if (level > 0) level--;
            continue;
        }
        if (id == OP_FRAME_SET) {
            int32_t s = ops[i].a;
            if (s >= 0 && s < LL_CONSTPROP_MAX_SLOTS) {
                int prev_id = (i > 0) ? (int)(intptr_t)ops[i - 1].handler : -1;
                if (i > 0 && !is_target[i] && !is_target[i - 1] && constprop_is_const_push(prev_id)) {
                    slots[s].level = level;
                    slots[s].kind = prev_id;
                    slots[s].value = (prev_id == OP_PUSH_INT) ? (*pool)[ops[i - 1].b] : 0;
                } else {
                    slots[s].level = -1;
                }
            }
            continue;
        }
        if (id == OP_VAR_REF_CELL) {
            int32_t depth = ops[i].a, s = ops[i].b;
            if (depth == 0 && s >= 0 && s < LL_CONSTPROP_MAX_SLOTS) slots[s].level = -1;
            continue;
        }
        if (id == OP_VAR_REF) {
            int32_t depth = ops[i].a, s = ops[i].b;
            if (depth == 0 && s >= 0 && s < LL_CONSTPROP_MAX_SLOTS && slots[s].level == level && !is_target[i]) {
                int32_t kind = slots[s].kind;
                ops[i].handler = (void *)(intptr_t)kind;
                ops[i].a = 0;
                ops[i].b = (kind == OP_PUSH_INT)
                    ? (int32_t)ll_irpass_pool_append(pool, pool_len, pool_cap, slots[s].value)
                    : 0;
                fired = 1;
            }
            continue;
        }
    }

    free(is_target);
    return fired;
}

void ll_irpass_register_constprop(void) {
    ll_irpass_register("constprop", constprop_run);
}
