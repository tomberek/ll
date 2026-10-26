/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 src/asm/ops/op_with.S -- see soft-wishing-engelbart.md plan
 *
 * OP_WITH_TRY's attrset probe reuses asnix_bshared_attrs_probe, the same
 * vendored helper op_select.c already calls -- it already handles both a
 * flat Attrs and a lazy `//` UnionHdr internally (binary search +
 * on-demand flatten/cache), so there is no need to hand-roll op_with.S's
 * own inlined binary search here. On a hit, the matched entry is forced
 * via asnix_force_for_print (same Cell/LazyApp/ThunkMap/Thunk unwrapping
 * op_select.c relies on) rather than re-deriving op_with.S's own inlined
 * force-loop by hand. */
#include "ops.h"
#include "builtins_shared.h"
#include "../env_helpers.h"

/* `with EXPR; BODY`: EXPR's value is already on top of VALSTACK (forced
 * if needed by whatever compiled it -- see compiler.c's AST_WITH case:
 * either a var-ref-cell, a thunk-free literal, or an OP_MK_THUNK). Binds
 * it into a fresh 1-slot Env frame and tags the frame's OWN `up` field
 * with ASNIX_ENV_WITH_TAG so OP_WITH_TRY/a depth-relative var-ref walk
 * can recognize it later. */
LL_OP_ATTR Value ll_op_with(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    Env *frame = ll_env_alloc(env, 1);
    frame->up |= ASNIX_ENV_WITH_TAG;
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
    if ((attrs.raw & ASNIX_TAG_MASK) != ASNIX_TAG_ATTRS) {
        static const char msg[] = "asnix: `with' value is not an attrset, aborting\n";
        asnix_fatal(msg, sizeof(msg) - 1);
    }
    uint32_t sym = (uint32_t)ip->b;
    Value out;
    int found = asnix_bshared_attrs_probe(attrs, sym, &out);
    if (!found) {
        ip = (IrOp *)((char *)ip + ip->a);
        LL_GOTO(ip, env, vs, pool, rs);
    }
    Value result = asnix_force_for_print(out);
    ll_check_valstack(vs);
    vs[0] = result;
    vs++;
    LL_NEXT(ip, env, vs, pool, rs);
}

/* All enclosing `with`s were tried (every OP_WITH_TRY took its "not
 * found" branch): unconditional fatal, the same error a compile-time-
 * unbound variable gets (compiler.c's AST_VAR case), just raised at
 * runtime since whether a `with`'s attrset actually has the key is a
 * runtime fact. */
LL_OP_ATTR Value ll_op_with_miss(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    (void)ip;
    (void)env;
    (void)vs;
    (void)pool;
    (void)rs;
    static const char msg[] = "asnix: undefined variable, aborting\n";
    asnix_fatal(msg, sizeof(msg) - 1);
}

/* No operands; undoes one OP_WITH: env = env->up with ASNIX_ENV_WITH_TAG
 * masked off first. This is OP_WITH's own counterpart to OP_FRAME_EXIT,
 * NOT a reuse of env_helpers.c's existing ll_env_frame_exit -- that
 * helper deliberately does NOT mask the tag bit (it only ever closes an
 * ordinary OP_FRAME_ALLOC frame, whose `up` never has the bit set in the
 * first place; see its own comment), so OP_WITH_EXIT needs its own
 * unmasking logic inline here, matching op_with.S's asnix_op_with_exit. */
LL_OP_ATTR Value ll_op_with_exit(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    env = (Env *)((uint8_t *)asnix_env_arena_base + (env->up & ~(uint32_t)ASNIX_ENV_WITH_TAG));
    LL_NEXT(ip, env, vs, pool, rs);
}
