/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445's src/asm/ops/op_select.S -- see ~/.claude/plans/soft-wishing-engelbart.md
 *
 * Cell/LazyApp/ThunkMap unwrapping happens inside ll_force_for_print
 * itself (closure_helpers.c), which this already calls on the
 * selected entry -- no separate resolve call needed here. Uses
 * ll_bshared_attrs_probe, which already handles both a flat Attrs
 * and a lazy `//` UnionHdr internally. */
#include "ops.h"
#include "builtins_shared.h"

LL_OP_ATTR Value ll_op_select(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    Value attrs = vs[-1];
    if ((attrs.raw & LL_TAG_MASK) != LL_TAG_ATTRS) {
        ll_bshared_select_type_fatal(attrs); /* _Noreturn */
    }
    uint32_t sym = (uint32_t)ip->a;
    Value out;
    int found = ll_bshared_attrs_probe(attrs, sym, &out);
    if (!found) {
        ll_bshared_select_missing_fatal(sym); /* _Noreturn */
    }
    Value result = ll_force_for_print(out);
    vs[-1] = result; /* pop attrs, push result -- net zero stack change */
    LL_NEXT(ip, env, vs, pool, rs);
}
