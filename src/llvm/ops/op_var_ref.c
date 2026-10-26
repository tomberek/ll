/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445's src/asm/ops/op_var_ref.S -- see ~/.claude/plans/soft-wishing-engelbart.md
 *
 * Resolves Cell/LazyApp/ThunkMap indirection (ll_resolve.c) before
 * checking the THUNK tag, matching op_var_ref.S's own
 * check-cell/chase/re-check-thunk loop: resolving one indirection can
 * reveal a Thunk that itself needs forcing, so the two checks
 * alternate until a genuine ground value or an UNFORCED thunk is
 * found. A FORCED thunk's cached value is never itself a Cell (see
 * op_var_ref.S's own .Lvar_ref_forced comment for the invariant proof)
 * so that path skips straight to push, same as the asm. */
#include "ops.h"

#include "../ll_resolve.h"
#include "value_util.h"

LL_OP_ATTR Value ll_op_var_ref(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    int64_t depth = ip->a, slot = ip->b;
    Value raw = asnix_env_lookup(env, depth, slot);
    raw = ll_resolve_indirection(raw);

    if ((raw.raw & ASNIX_TAG_MASK) != ASNIX_TAG_THUNK) {
        ll_check_valstack(vs);
        vs[0] = raw;
        vs++;
        LL_NEXT(ip, env, vs, pool, rs);
    }

    Thunk *t = (Thunk *)ll_untag(raw);
    int64_t state_word = t->state;
    int64_t state_tag = state_word & ASNIX_TAG_MASK;

    if (state_tag == ASNIX_THUNK_STATE_FORCED) {
        Value cached;
        cached.raw = t->env;
        ll_check_valstack(vs);
        vs[0] = cached;
        vs++;
        LL_NEXT(ip, env, vs, pool, rs);
    }
    if (state_tag == ASNIX_THUNK_STATE_IN_PROGRESS) {
        static const char msg[] = "asnix: infinite recursion forcing a thunk (blackhole), aborting\n";
        asnix_fatal(msg, sizeof(msg) - 1);
    }
    if (state_tag == ASNIX_THUNK_STATE_FAILED) {
        static const char msg[] = "asnix: re-evaluation of a thunk whose first evaluation failed, aborting\n";
        asnix_fatal(msg, sizeof(msg) - 1);
    }

    /* UNFORCED: state_word IS the clean IrProgram* (low tag bits 0). */
    IrProgram *body_prog = (IrProgram *)(uintptr_t)state_word;
    t->state = ASNIX_THUNK_STATE_IN_PROGRESS;

    ll_check_retsp(rs);
    rs[0].ip = ip + 1;
    rs[0].env = env;
    rs[0].pool = pool;
    rs[0].force_target = t;
    rs++;

    Env *thunk_env = (Env *)(uintptr_t)t->env;
    t->env = (uint64_t)(uintptr_t)asnix_inprogress_head;
    asnix_inprogress_head = t;

    env = thunk_env;
    pool = body_prog->pool;
    ip = body_prog->ops;
    [[clang::musttail]] return ((LLOpFn)ip->handler)(ip, env, vs, pool, rs);
}

/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445's src/asm/ops/op_var_ref_raw.S --
 * pushes the slot's raw content verbatim, with NO forcing at all (not
 * even thunk-forcing) -- needed by asnix_bshared_make_curried_native_noforce
 * (builtins_shared.c), which root_env.c's merged builtins attrset
 * reaches during construction even for a trivial top-level eval. */
LL_OP_ATTR Value ll_op_var_ref_raw(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    int64_t depth = ip->a, slot = ip->b;
    Value raw = asnix_env_lookup(env, depth, slot);
    ll_check_valstack(vs);
    vs[0] = raw;
    vs++;
    LL_NEXT(ip, env, vs, pool, rs);
}

/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445's src/asm/ops/op_var_ref_cell.S --
 * pushes a Cell wrapping a pointer straight at the target Env slot
 * (not its value), emitted only by compiler.c's try_cell_ref for a
 * bare-variable sub-expression about to be thunked -- see heap.h's
 * ASNIX_CELL_MARKER comment. Same depth-walk env.h's own
 * asnix_env_lookup does internally, replicated here because this op
 * needs the slot's ADDRESS, not its dereferenced value; env.h's own
 * asnix_env_arena_base/ASNIX_ENV_WITH_TAG are both already vendored
 * and declared for exactly this kind of walk. */
LL_OP_ATTR Value ll_op_var_ref_cell(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    int64_t depth = ip->a, slot = ip->b;
    Env *e = env;
    for (int64_t d = 0; d < depth; d++) {
        uintptr_t up = e->up & ~(uintptr_t)ASNIX_ENV_WITH_TAG;
        e = (Env *)((uint8_t *)asnix_env_arena_base + up);
    }
    Value cell = asnix_make_cell_value(&e->values[slot]);
    ll_check_valstack(vs);
    vs[0] = cell;
    vs++;
    LL_NEXT(ip, env, vs, pool, rs);
}
