/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445's src/asm/ops/op_select.S -- see ~/.claude/plans/soft-wishing-engelbart.md
 *
 * Scope matches today's ll: no Cell unwrapping (documented gap there
 * too). Uses asnix_bshared_attrs_probe, which already handles both a
 * flat Attrs and a lazy `//` UnionHdr internally. */
#include "ops.h"
#include "builtins_shared.h"

Value ll_op_select(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    Value attrs = vs[-1];
    if ((attrs.raw & ASNIX_TAG_MASK) != ASNIX_TAG_ATTRS) {
        asnix_bshared_select_type_fatal(attrs); /* _Noreturn */
    }
    uint32_t sym = (uint32_t)ip->a;
    Value out;
    int found = asnix_bshared_attrs_probe(attrs, sym, &out);
    if (!found) {
        asnix_bshared_select_missing_fatal(sym); /* _Noreturn */
    }
    Value result = asnix_force_for_print(out);
    vs[-1] = result; /* pop attrs, push result -- net zero stack change */
    LL_NEXT(ip, env, vs, pool, rs);
}
