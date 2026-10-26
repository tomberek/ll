/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445's src/asm/ops/op_mk_attrs.S -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "ops.h"
#include "builtins_shared.h"

/* Fixed-size scratch, not a VLA: a variably-modified local would block
 * musttail at the end of this function. Covers every realistic
 * literal attrset (bench/workloads/attrset_build_large.nix's ~2,000
 * entries included) with room to spare; a literal larger than this
 * falls back to a one-shot, never-freed arena scratch buffer instead
 * (this evaluator's existing allocation philosophy, see arena.h). */
#define LL_MK_ATTRS_FIXED_SYMS 8192

LL_OP_ATTR Value ll_op_mk_attrs(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    int64_t count = ip->a;
    int64_t pidx = ip->b;
    Value *vals = vs - count; /* the popped values already sit contiguously here */

    uint32_t fixed_syms[LL_MK_ATTRS_FIXED_SYMS];
    uint32_t *syms = fixed_syms;
    if (count > LL_MK_ATTRS_FIXED_SYMS) {
        syms = (uint32_t *)ll_arena_alloc((uint64_t)count * sizeof(uint32_t));
    }
    for (int64_t i = 0; i < count; i++) syms[i] = (uint32_t)pool[pidx + i];
    Value result = asnix_bshared_make_attrs(syms, vals, count);
    vs = vals;
    ll_check_valstack(vs);
    vs[0] = result;
    vs++;
    LL_NEXT(ip, env, vs, pool, rs);
}
