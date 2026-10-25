/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "asnix/irpass.h"

#include <stdint.h>

/* Proof-of-concept pass: folds a `OP_PUSH_INT a; OP_PUSH_INT b; OP_<binop>`
 * triple into a single OP_PUSH_INT/OP_PUSH_TRUE/OP_PUSH_FALSE, for every
 * pure int-int binary op this IR has whose result depends only on its two
 * operands (ADD/SUB/MUL/DIV and the 6 ordering/equality comparisons) --
 * compiler.c already constant-folds int-literal `+`/`-`/`*` at the AST
 * level in some cases, but NOT uniformly across every arithmetic/
 * comparison op or through intermediate binding boundaries the AST-level
 * fold doesn't see (e.g. a constant produced by inlining, or by this same
 * pass's own prior iteration landing two now-adjacent constants next to
 * each other) -- this is the bytecode-level backstop, and (since it works
 * on OP_PUSH_INT regardless of how it got there) also now covers any
 * actual source-level `1 + 2` the AST-level fold misses.
 *
 * OP_DIV is skipped whenever the divisor is 0, or the pair is
 * (INT64_MIN, -1) -- both must still reach the runtime op (abort with
 * "division by zero", and avoid signed-overflow UB respectively), not be
 * silently folded away.
 *
 * M169: OP_PUSH_INT no longer carries its literal inline in `a` (IrOp's
 * fields are now only 32 bits, too narrow for an arbitrary int64 literal)
 * -- both operands are now read from the pool via each op's own `b`
 * (pool index), same as OP_PUSH_CONST. The fold result is written back
 * IN PLACE into ops[i]'s own already-owned pool slot (ops[i].b is left
 * unchanged) rather than appending a new pool entry -- that slot isn't
 * shared with any other op, so overwriting it is safe and needs no pool
 * growth (`pool_len`/`pool_cap` stay unused here, same as before). */
static int fold_constant_int_binop(IrOp *ops, size_t *len, int64_t **pool, size_t *pool_len, size_t *pool_cap) {
    (void)pool_len;
    (void)pool_cap;
    for (size_t i = 0; i + 2 < *len; i++) {
        if ((int)(intptr_t)ops[i].handler != OP_PUSH_INT) continue;
        if ((int)(intptr_t)ops[i + 1].handler != OP_PUSH_INT) continue;
        int op_id = (int)(intptr_t)ops[i + 2].handler;
        int64_t a = (*pool)[ops[i].b], b = (*pool)[ops[i + 1].b];
        int64_t result = 0;
        int is_bool = 0, ok = 1;
        switch (op_id) {
            /* `__builtin_*_overflow`, not plain `+`/`-`/`*` -- found by
             * actually running the pipeline, not assumed safe: this
             * switch originally used plain C int64 arithmetic with NO
             * overflow check at all, a real latent miscompile bug since
             * M120 (`fold always succeeds, even when the real runtime op
             * would have aborted`), simply never triggered before because
             * nothing upstream ever produced two adjacent OP_PUSH_INT ops
             * around ADD/SUB/MUL from a non-literal source -- confirmed on
             * the real nix-conformance corpus: `let a = 4294967297; in a *
             * a * a` (eval-fail-overflowing-mul.nix) WRONGLY succeeded
             * under ASNIX_IR_OPT=1 before this fix, once this session's
             * own constprop pass started feeding it exactly that shape.
             * Mirrors compiler.c's own try_fold_const_binop (M136) exactly
             * -- same three builtins, same bounds check below. */
            case OP_ADD: if (__builtin_add_overflow(a, b, &result)) ok = 0; break;
            case OP_SUB: if (__builtin_sub_overflow(a, b, &result)) ok = 0; break;
            case OP_MUL: if (__builtin_mul_overflow(a, b, &result)) ok = 0; break;
            case OP_DIV:
                if (b == 0) { ok = 0; break; }
                if (a == INT64_MIN && b == -1) { ok = 0; break; }
                result = a / b; /* C's / already truncates toward zero, matching OP_DIV's own semantics */
                break;
            case OP_EQ: result = (a == b); is_bool = 1; break;
            case OP_NEQ: result = (a != b); is_bool = 1; break;
            case OP_LT: result = (a < b); is_bool = 1; break;
            case OP_GT: result = (a > b); is_bool = 1; break;
            case OP_LE: result = (a <= b); is_bool = 1; break;
            case OP_GE: result = (a >= b); is_bool = 1; break;
            default: ok = 0; break;
        }
        /* The narrower ASNIX_INT_MIN/MAX tagged-value window, same
         * as compiler.c's try_fold_const_binop -- a result that survives
         * the int64-overflow check above can still fall outside what a
         * real Nix int VALUE can represent (op_arith.S's own range check,
         * see value.h), which must also still reach the runtime op and
         * abort, not be silently folded into an out-of-range constant. */
        if (ok && !is_bool && (result < ASNIX_INT_MIN || result > ASNIX_INT_MAX)) ok = 0;
        if (!ok) continue;
        if (is_bool) {
            ops[i].handler = (void *)(intptr_t)(result ? OP_PUSH_TRUE : OP_PUSH_FALSE);
            ops[i].a = 0;
            ops[i].b = 0;
        } else {
            ops[i].handler = (void *)(intptr_t)OP_PUSH_INT;
            (*pool)[ops[i].b] = result; /* overwrite ops[i]'s own pool slot in place */
            ops[i].a = 0;
            /* ops[i].b intentionally left as-is: same pool slot, now holding `result` */
        }
        asnix_irpass_remove(ops, len, i + 1, 2);
        return 1; /* re-scan from the top next iteration -- simplest correct strategy */
    }
    return 0;
}

void asnix_irpass_register_cfold(void) {
    asnix_irpass_register("cfold", fold_constant_int_binop);
}
