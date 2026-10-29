/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 src/asm/ops/op_has_attr.S,
 * src/asm/ops/op_has_attr_if.S, src/asm/ops/op_has_attr_dynamic.S -- see
 * soft-wishing-engelbart.md plan
 *
 * All three are pure membership tests (the Nix `?` operator): a
 * non-attrset target is NEVER fatal (just FALSE, or the else-branch
 * for the _IF form), and a present-but-unforced entry is NEVER forced
 * -- this only asks "is the key there", never "what is its value".
 * ll_bshared_attrs_probe already handles both a flat Attrs and a
 * lazy `//` UnionHdr internally (same helper op_select.c's OP_SELECT
 * uses), so no separate resolve call is needed here. */
#include "ops.h"
#include "builtins_shared.h"

LL_OP_ATTR Value ll_op_has_attr(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    Value attrs = vs[-1];
    if ((attrs.raw & LL_TAG_MASK) != LL_TAG_ATTRS) {
        vs[-1] = ll_false();
        LL_NEXT(ip, env, vs, pool, rs);
    }
    uint32_t sym = (uint32_t)ip->a;
    Value out;
    int found = ll_bshared_attrs_probe(attrs, sym, &out);
    vs[-1] = found ? ll_true() : ll_false();
    LL_NEXT(ip, env, vs, pool, rs);
}

LL_OP_ATTR Value ll_op_has_attr_if(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    Value attrs = vs[-1];
    vs--;
    if ((attrs.raw & LL_TAG_MASK) != LL_TAG_ATTRS) {
        ip = (IrOp *)((char *)ip + ip->a);
        LL_GOTO(ip, env, vs, pool, rs);
    }
    uint32_t sym = (uint32_t)ip->b;
    Value out;
    int found = ll_bshared_attrs_probe(attrs, sym, &out);
    if (found) {
        ip++;
    } else {
        ip = (IrOp *)((char *)ip + ip->a);
    }
    LL_GOTO(ip, env, vs, pool, rs);
}

LL_OP_ATTR Value ll_op_has_attr_dynamic(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    Value name = vs[-1], target = vs[-2];
    vs -= 2;
    int found = ll_bshared_has_attr_dynamic(target, name);
    vs[0] = found ? ll_true() : ll_false();
    vs++;
    LL_NEXT(ip, env, vs, pool, rs);
}
