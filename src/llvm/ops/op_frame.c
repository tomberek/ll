/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445's src/asm/ops/op_frame.S -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "ops.h"
#include "../env_helpers.h"

LL_OP_ATTR Value ll_op_frame_alloc(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    int64_t nslots = ip->a;
    env = ll_env_alloc(env, nslots); /* slots left uninitialized -- every slot is
                                          * written by an OP_FRAME_SET before anything
                                          * can read it (see compiler.c) */
    LL_NEXT(ip, env, vs, pool, rs);
}

LL_OP_ATTR Value ll_op_frame_set(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    int64_t slot = ip->a;
    vs--;
    ll_env_set(env, slot, vs[0]);
    LL_NEXT(ip, env, vs, pool, rs);
}

LL_OP_ATTR Value ll_op_frame_exit(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    env = ll_env_frame_exit(env);
    LL_NEXT(ip, env, vs, pool, rs);
}

/* a = slot index (M29); pushes the CURRENT Env's slot verbatim, raw
 * data movement, deliberately NOT forcing -- used only to read back a
 * `rec {}` binding's freshly-OP_FRAME_SET thunk for attrset
 * construction, where OP_VAR_REF's force-on-read would be wrong (it
 * would eagerly force every `rec {}` field the instant the literal is
 * constructed, not lazily on first actual select). */
LL_OP_ATTR Value ll_op_frame_get(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    int64_t slot = ip->a;
    ll_check_valstack(vs);
    vs[0] = env->values[slot];
    vs++;
    LL_NEXT(ip, env, vs, pool, rs);
}
