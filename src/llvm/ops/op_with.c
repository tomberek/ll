/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 src/asm/ops/op_with.S -- see soft-wishing-engelbart.md plan */
#include "ops.h"
#include "builtins_shared.h"
#include "../env_helpers.h"

/* `with EXPR; BODY`: EXPR's value is already on top of VALSTACK. Binds
 * it into a fresh 1-slot Env frame and tags the frame's OWN `up` field
 * with LL_ENV_WITH_TAG so OP_WITH_TRY/a depth-relative var-ref walk
 * can recognize it later. */
LL_OP_ATTR Value ll_op_with(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    Env *frame = ll_env_alloc_fast(env, 1);
    frame->up |= LL_ENV_WITH_TAG;
    vs--;
    ll_env_set(frame, 0, vs[0]);
    env = frame;
    LL_NEXT(ip, env, vs, pool, rs);
}

/* a = self-relative byte delta to jump to when symbol `b` isn't present
 * in this with-level's attrs (try the next enclosing `with`, or fall
 * into OP_WITH_MISS if this was the last one); b = symbol id. Pops the
 * attrs value a preceding OP_VAR_REF already forced and pushed. Fatal if
 * it isn't an attrset -- matches real Nix: a non-attrset with-value only
 * errors once it's actually consulted. On a hit, forces the matching
 * entry (attrset binding values are lazy, same as OP_SELECT) and falls
 * through with it on top of VALSTACK; compiler.c always emits an
 * unconditional OP_JMP right after this op for exactly that fallthrough
 * to land on. */
LL_OP_ATTR Value ll_op_with_try(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    Value attrs = vs[-1];
    vs--;
    if ((attrs.raw & LL_TAG_MASK) != LL_TAG_ATTRS) {
        static const char msg[] = "ll: `with' value is not an attrset, aborting\n";
        ll_fatal(msg, sizeof(msg) - 1);
    }
    uint32_t sym = (uint32_t)ip->b;
    Value out;
    int found = ll_bshared_attrs_probe(attrs, sym, &out);
    if (!found) {
        ip = (IrOp *)((char *)ip + ip->a);
        LL_GOTO(ip, env, vs, pool, rs);
    }
    Value result = ll_force_for_print(out);
    ll_check_valstack(vs);
    vs[0] = result;
    vs++;
    LL_NEXT(ip, env, vs, pool, rs);
}

/* All enclosing `with`s were tried: unconditional fatal. */
LL_OP_ATTR Value ll_op_with_miss(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    (void)ip;
    (void)env;
    (void)vs;
    (void)pool;
    (void)rs;
    static const char msg[] = "ll: undefined variable, aborting\n";
    ll_fatal(msg, sizeof(msg) - 1);
}

/* No operands; undoes one OP_WITH: env = env->up with LL_ENV_WITH_TAG
 * masked off first. This is NOT a reuse of env_helpers.c's existing
 * ll_env_frame_exit -- that helper deliberately does NOT mask the tag
 * bit (it only ever closes an ordinary OP_FRAME_ALLOC frame, whose `up`
 * never has the bit set in the first place), so this op needs its own
 * unmasking logic inline here. */
LL_OP_ATTR Value ll_op_with_exit(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    env = (Env *)((uint8_t *)ll_env_arena_base + (env->up & ~(uint32_t)LL_ENV_WITH_TAG));
    LL_NEXT(ip, env, vs, pool, rs);
}
