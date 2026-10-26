/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445's
 * src/asm/ops/op_select_or.S, src/asm/ops/op_select_dynamic.S,
 * src/asm/ops/op_select_dynamic_or.S -- see
 * ~/.claude/plans/soft-wishing-engelbart.md
 *
 * OP_SELECT_OR mirrors op_select.c's own ll_op_select (same
 * asnix_bshared_attrs_probe + asnix_force_for_print pair, which already
 * resolves Cell/LazyApp/ThunkMap for the hit value), but a miss -- OR a
 * non-attrset target -- is NOT fatal here: it's a self-relative byte-
 * delta jump to the compiler-emitted default expression's IR (ip->a),
 * same self-relative-to-THIS-ip convention op_if.c's OP_IF/OP_JMP
 * already established (LL_GOTO, never LL_NEXT, after computing the new
 * ip -- using LL_NEXT here would double-advance).
 *
 * OP_SELECT_DYNAMIC/OP_SELECT_DYNAMIC_OR both defer entirely to their
 * vendored C helpers (asnix_bshared_select_dynamic/
 * asnix_bshared_select_dynamic_or, builtins_shared.c), which already
 * intern the runtime name, binary-search, and force an unforced thunk
 * hit -- no Cell/LazyApp/ThunkMap handling needed here, it's inside the
 * helpers. Both pop the name Value (pushed last by compiler.c's
 * AST_SELECT case) then the select target -- reverse of push order. */
#include "ops.h"
#include "builtins_shared.h"

LL_OP_ATTR Value ll_op_select_or(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    Value attrs = vs[-1];
    if ((attrs.raw & ASNIX_TAG_MASK) != ASNIX_TAG_ATTRS) {
        /* not even an attrset -- not fatal, see header comment */
        vs--;
        ip = (IrOp *)((char *)ip + ip->a);
        LL_GOTO(ip, env, vs, pool, rs);
    }
    uint32_t sym = (uint32_t)ip->b;
    Value out;
    int found = asnix_bshared_attrs_probe(attrs, sym, &out);
    if (!found) {
        vs--;
        ip = (IrOp *)((char *)ip + ip->a);
        LL_GOTO(ip, env, vs, pool, rs);
    }
    Value result = asnix_force_for_print(out);
    vs[-1] = result; /* pop attrs, push result -- net zero stack change */
    LL_NEXT(ip, env, vs, pool, rs);
}

LL_OP_ATTR Value ll_op_select_dynamic(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    Value name = vs[-1]; /* pushed last, popped first */
    Value target = vs[-2];
    Value result = asnix_bshared_select_dynamic(target, name);
    vs--; /* net: pop both operands, push one result */
    vs[-1] = result;
    LL_NEXT(ip, env, vs, pool, rs);
}

LL_OP_ATTR Value ll_op_select_dynamic_or(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    Value name = vs[-1]; /* pushed last, popped first */
    Value target = vs[-2];
    Value out;
    int found = asnix_bshared_select_dynamic_or(target, name, &out);
    vs -= 2; /* pop both operands unconditionally */
    if (!found) {
        ip = (IrOp *)((char *)ip + ip->a);
        LL_GOTO(ip, env, vs, pool, rs);
    }
    vs[0] = out;
    vs++;
    LL_NEXT(ip, env, vs, pool, rs);
}
