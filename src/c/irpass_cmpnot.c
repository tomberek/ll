/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "ll/irpass.h"

#include <stdint.h>

/* Fuses a comparison immediately followed by OP_NOT into the complementary
 * comparison, dropping the NOT (2 ops -> 1):
 *
 *   OP_EQ ; OP_NOT  ->  OP_NEQ
 *   OP_NEQ; OP_NOT  ->  OP_EQ
 *
 * ONLY these two. The four ordering-comparison pairs this pass was
 * originally asked to also handle --
 *
 *   OP_LT ; OP_NOT  ->  OP_GE
 *   OP_GT ; OP_NOT  ->  OP_LE
 *   OP_LE ; OP_NOT  ->  OP_GT
 *   OP_GE ; OP_NOT  ->  OP_LT
 *
 * -- were investigated and DECLINED: they are unsafe in general, not just
 * theoretically. Reading op_cmp.S (not just ir.h's own per-op comment,
 * which is stale here -- it says LT/GT/LE/GE "both must be TAG_INT, else
 * fatal", but the actual handler has a slow path for floats AND strings,
 * never fataling on those) shows OP_LT/OP_GT/OP_LE/OP_GE's slow path calls
 * ll_cmp_{lt,gt,le,ge}_values (value_util.c), each of which does a
 * direct IEEE `a < b` / `a > b` / `a <= b` / `a >= b` on two doubles.
 * IEEE754 comparisons involving a NaN operand are famously NOT a total
 * order: every one of `<`, `>`, `<=`, `>=` returns false when either
 * operand is NaN, so e.g. `NOT(LT(x,y))` is unconditionally TRUE whenever
 * either operand is NaN, while the "complementary" `GE(x,y)` computed
 * directly is FALSE for the exact same NaN inputs -- NOT(LT) and GE
 * diverge whenever a NaN reaches either operand. Same story for the
 * GT/LE pair. This is confirmed reachable in practice in this
 * interpreter, not just a hypothetical: neither ll_add_values (value_
 * util.c, float+float case) nor ll_div_values' zero-divisor check
 * guards against the *result* being NaN, and builtins.fromJSON's number
 * parser (builtins_json.c's parse_json_number) feeds strtod() with no
 * overflow check, so e.g.
 *   let n = builtins.fromJSON "1e400" + builtins.fromJSON "-1e400";
 *   in n <= 0
 * produces a real NaN Float Value at runtime (verified directly: `toString
 * n` prints "-nan"). Confirmed empirically on this exact pair: with
 * un-fused IR, `0 < n` is `false` and `!(0 < n)` is `true` (matching real
 * Nix's own definition of `<=`/`>=`/`>` as literally `!(__lessThan ...)` --
 * see compiler.c's AST_BINOP case, which is why asnix's compiler already
 * special-cases `>`/`<=` as swapped-operand `OP_LT`+`OP_NOT` rather than
 * ever emitting the native OP_GT/OP_LE at all), but asnix's OWN native
 * OP_GE on the same two operands independently evaluates to `false` --
 * i.e. fusing `OP_LT;OP_NOT` into `OP_GE` would silently replace the
 * CORRECT `!(0 < n)` with the ALREADY-DIVERGENT-FROM-upstream-Nix native
 * OP_GE, turning currently-correct compiled code (every `<=`/`>` in any
 * source program compiles to exactly `OP_LT;OP_NOT`/`OP_LT`, see compiler.
 * c's comment on this) into code that gives a different, wrong answer the
 * moment a NaN float reaches it. `OP_GE;OP_NOT -> OP_LT` has the exact
 * mirror-image bug (verified the same way with `0 >= n`/`0 < n`). This
 * isn't a problem for EQ/NEQ: both compile to a single call to the SAME
 * underlying C function (ll_bshared_value_eq_op) with NEQ's own asm
 * just flipping the boolean result of that one call (op_cmp.S's
 * ll_op_eq/ll_op_neq -- literally the same `call`, opposite
 * cmoveq/final-mapping), so NOT(EQ) == NEQ exactly, unconditionally, by
 * construction -- no float/NaN gray zone, no domain restriction at all
 * (EQ/NEQ's scope is deliberately wider than ordering comparisons, per
 * op_cmp.S's own header comment: numeric equality, IMM-subtag equality,
 * or pointer identity, with no "fatal" case whatsoever). OP_NOT's own
 * precondition ("top of stack must be TAG_IMM true/false, else fatal",
 * ir.h) is a non-issue for either kept rule: EQ/NEQ unconditionally push
 * TRUE/FALSE, so the preceding op already guarantees OP_NOT's
 * precondition holds. */
/* Found on a REAL nixpkgs workload (not caught by the 308-fixture +
 * 806-file nix-conformance sweep this pass was originally verified
 * against): `OP_NOT` at i+1 is not always exclusively fed by ops[i]'s own
 * result. `!(if cond then true else (y != []))` compiles to an ordinary
 * if/else whose shared join point -- the code textually AFTER the if,
 * which is simply this source's own `!` -- happens to be this exact
 * `OP_NOT`: the "then" branch pushes PUSH_TRUE and JMPs directly to that
 * NOT (skipping the "else" branch's own NEQ entirely), while the "else"
 * branch's NEQ falls through into the SAME NOT. Fusing here would rewrite
 * ops[i] (the else branch's NEQ) to EQ and delete the NOT -- silently
 * dropping the negation the "then" branch's jump was relying on.
 * ll_irpass_remove's own safety check caught this (a live jump
 * targets exactly the op being removed) and aborted rather than
 * miscompile, which is how this was found at all. Fix: only fuse when
 * nothing else jumps directly to the NOT -- same incoming-jump guard
 * irpass_dce_if.c's own range_has_incoming_jump establishes for exactly
 * this class of hazard (duplicated here rather than shared, to keep this
 * pass self-contained in its own file per this framework's convention).
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
        int64_t target = (int64_t)i + ops[i].a / LL_IROP_SIZE;
        if (target == (int64_t)index) return 1;
    }
    return 0;
}

static int fuse_cmp_not(IrOp *ops, size_t *len, int64_t **pool, size_t *pool_len, size_t *pool_cap) {
    (void)pool;
    (void)pool_len;
    (void)pool_cap; /* M118 strconcat-fold extension widened LlIrPass's signature for every
                      * pass uniformly (see irpass.h) -- this pass only fuses EQ/NEQ;NOT pairs,
                      * never touches the constant pool. */
    for (size_t i = 0; i + 1 < *len; i++) {
        int op_id = (int)(intptr_t)ops[i].handler;
        int fused;
        switch (op_id) {
            case OP_EQ: fused = OP_NEQ; break;
            case OP_NEQ: fused = OP_EQ; break;
            default: continue;
        }
        if ((int)(intptr_t)ops[i + 1].handler != OP_NOT) continue;
        if (is_jump_target(ops, *len, i + 1)) continue; /* NOT is a shared join point -- don't fuse */
        ops[i].handler = (void *)(intptr_t)fused;
        ll_irpass_remove(ops, len, i + 1, 1);
        return 1; /* re-scan from the top next iteration -- simplest correct strategy */
    }
    return 0;
}

void ll_irpass_register_cmpnot(void) {
    ll_irpass_register("cmpnot", fuse_cmp_not);
}
