/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "ll/irpass.h"

#include <stdint.h>

/* Fuses a comparison immediately followed by OP_NOT into the complementary
 * comparison, dropping the NOT (2 ops -> 1):
 *
 *   OP_EQ ; OP_NOT  ->  OP_NEQ
 *   OP_NEQ; OP_NOT  ->  OP_EQ
 *
 * ONLY these two. The four ordering-comparison pairs (LT/NOT->GE,
 * GT/NOT->LE, LE/NOT->GT, GE/NOT->LT) are DELIBERATELY NOT fused: OP_LT/
 * OP_GT/OP_LE/OP_GE's slow path does a direct IEEE `<`/`>`/`<=`/`>=` on
 * two doubles (op_cmp.S -> ll_cmp_{lt,gt,le,ge}_values), and IEEE754
 * comparisons are not a total order under NaN -- every one of those
 * operators is false when either operand is NaN, so `NOT(LT(x,y))` is
 * unconditionally TRUE for a NaN operand while the "complementary"
 * `GE(x,y)` computed directly is FALSE for the same inputs. This is
 * reachable in practice, not just theoretically: builtins.fromJSON's
 * number parser feeds strtod() with no overflow check, so `fromJSON
 * "1e400" + fromJSON "-1e400"` produces a real NaN Float at runtime, and
 * fusing `OP_LT;OP_NOT` into `OP_GE` would silently replace a
 * currently-correct `!(0 < n)` with the DIVERGENT native OP_GE
 * (`OP_GE;OP_NOT -> OP_LT` has the mirror-image bug). EQ/NEQ have no such
 * hazard: both compile to a single call to the same underlying
 * ll_bshared_value_eq_op, with NEQ's own asm just flipping the boolean
 * result, so NOT(EQ) == NEQ exactly, unconditionally, by construction.
 * OP_NOT's own precondition ("top of stack must be TAG_IMM true/false,
 * else fatal") is a non-issue for either kept rule: EQ/NEQ unconditionally
 * push TRUE/FALSE.
 *
 * `OP_NOT` at i+1 is not always exclusively fed by ops[i]'s own result:
 * found on a real nixpkgs workload, `!(if cond then true else (y != []))`
 * has a shared join point where the "then" branch JMPs directly to the
 * same OP_NOT that the "else" branch's NEQ falls through into. Fusing
 * unconditionally would rewrite the else branch's NEQ to EQ and delete
 * the NOT, silently dropping the negation the "then" branch's jump relies
 * on. Guard: only fuse when nothing else jumps directly to the NOT --
 * same incoming-jump guard irpass_dce_if.c's own range_has_incoming_jump
 * establishes for this same hazard class. */
static int op_is_jump(int op_id) {
    return op_id == OP_IF || op_id == OP_JMP || op_id == OP_WITH_TRY || op_id == OP_SELECT_OR ||
           op_id == OP_SELECT_DYNAMIC_OR || op_id == OP_EQ_IF || op_id == OP_NEQ_IF || op_id == OP_LT_IF ||
           op_id == OP_GE_IF || op_id == OP_HAS_ATTR_IF;
}

static int is_jump_target(IrOp *ops, size_t len, size_t index) {
    for (size_t i = 0; i < len; i++) {
        int id = (int)(intptr_t)ops[i].handler;
        if (!op_is_jump(id)) continue;
        int64_t target = (int64_t)i + ops[i].a / LL_IROP_SIZE;
        if (target == (int64_t)index) return 1;
    }
    return 0;
}

static int fuse_cmp_not(IrOp *ops, size_t *len, int64_t **pool, size_t *pool_len, size_t *pool_cap) {
    (void)pool;
    (void)pool_len;
    (void)pool_cap;
    for (size_t i = 0; i + 1 < *len; i++) {
        int op_id = (int)(intptr_t)ops[i].handler;
        int fused;
        switch (op_id) {
            case OP_EQ: fused = OP_NEQ; break;
            case OP_NEQ: fused = OP_EQ; break;
            default: continue;
        }
        if ((int)(intptr_t)ops[i + 1].handler != OP_NOT) continue;
        if (is_jump_target(ops, *len, i + 1)) continue;
        ops[i].handler = (void *)(intptr_t)fused;
        ll_irpass_remove(ops, len, i + 1, 1);
        return 1;
    }
    return 0;
}

void ll_irpass_register_cmpnot(void) {
    ll_irpass_register("cmpnot", fuse_cmp_not);
}
