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
#include "tryeval.h"
#include "value_util.h"

LL_OP_ATTR Value ll_op_var_ref(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    int64_t depth = ip->a, slot = ip->b;
    Value raw = ll_env_lookup_fast(env, depth, slot);
    raw = ll_resolve_indirection(raw);

    if ((raw.raw & LL_TAG_MASK) != LL_TAG_THUNK) {
        ll_check_valstack(vs);
        vs[0] = raw;
        vs++;
        LL_NEXT(ip, env, vs, pool, rs);
    }

    Thunk *t = (Thunk *)ll_untag(raw);
    int64_t state_word = t->state;
    int64_t state_tag = state_word & LL_TAG_MASK;

    if (state_tag == LL_THUNK_STATE_FORCED) {
        Value cached;
        cached.raw = t->env;
        ll_check_valstack(vs);
        vs[0] = cached;
        vs++;
        LL_NEXT(ip, env, vs, pool, rs);
    }
    if (state_tag == LL_THUNK_STATE_IN_PROGRESS) {
        static const char msg[] = "ll: infinite recursion forcing a thunk (blackhole), aborting\n";
        ll_fatal(msg, sizeof(msg) - 1);
    }
    if (state_tag == LL_THUNK_STATE_FAILED) {
        /* M82: matches op_var_ref.S's own .Lvar_ref_reraise -- route
         * through ll_tryeval_raise_or_fatal (tryeval.c), not
         * ll_fatal directly, so re-forcing a thunk whose first
         * evaluation failed inside an active builtins.tryEval
         * re-raises (catchable again) instead of unconditionally
         * aborting the whole process. Falls back to exactly
         * ll_fatal's own write+exit(1) behavior when no tryEval is
         * active. */
        static const char msg[] = "ll: re-evaluation of a thunk whose first evaluation failed, aborting\n";
        ll_tryeval_raise_or_fatal(msg, sizeof(msg) - 1); /* _Noreturn */
    }

    /* UNFORCED: state_word IS the clean IrProgram* (low tag bits 0). */
    IrProgram *body_prog = (IrProgram *)(uintptr_t)state_word;
    t->state = LL_THUNK_STATE_IN_PROGRESS;

    ll_check_retsp(rs);
    rs[0].ip = ip + 1;
    rs[0].env = env;
    rs[0].pool = pool;
    rs[0].force_target = t;
    rs++;

    Env *thunk_env = (Env *)(uintptr_t)t->env;
    t->env = (uint64_t)(uintptr_t)ll_inprogress_head;
    ll_inprogress_head = t;

    env = thunk_env;
    pool = body_prog->pool;
    ip = body_prog->ops;
    [[clang::musttail]] return ((LLOpFn)ip->handler)(ip, env, vs, pool, rs);
}

/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445's src/asm/ops/op_var_ref_raw.S --
 * pushes the slot's raw content verbatim, with NO forcing at all (not
 * even thunk-forcing) -- needed by ll_bshared_make_curried_native_noforce
 * (builtins_shared.c), which root_env.c's merged builtins attrset
 * reaches during construction even for a trivial top-level eval. */
LL_OP_ATTR Value ll_op_var_ref_raw(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    int64_t depth = ip->a, slot = ip->b;
    Value raw = ll_env_lookup_fast(env, depth, slot);
    ll_check_valstack(vs);
    vs[0] = raw;
    vs++;
    LL_NEXT(ip, env, vs, pool, rs);
}

/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445's src/asm/ops/op_var_ref_cell.S --
 * pushes a Cell wrapping a pointer straight at the target Env slot
 * (not its value), emitted only by compiler.c's try_cell_ref for a
 * bare-variable sub-expression about to be thunked -- see heap.h's
 * LL_CELL_MARKER comment. Same depth-walk env.h's own
 * ll_env_lookup does internally, replicated here because this op
 * needs the slot's ADDRESS, not its dereferenced value; env.h's own
 * ll_env_arena_base/LL_ENV_WITH_TAG are both already vendored
 * and declared for exactly this kind of walk.
 *
 * M-skip-cell: a Cell is only ever needed to defer the READ itself, for
 * the rare case where this slot is still unpopulated (a same-frame
 * mutually-recursive `let` referencing a not-yet-built sibling) --
 * compiler.c's try_cell_ref (ScopeFrame.has_overrides) already refuses
 * to ever emit THIS op for a reference into a `rec{}` with `__overrides`
 * (a SEPARATE hazard: ib_rec_overrides_merge rewrites an already-
 * populated sibling slot to a DIFFERENT object after the fact, which a
 * Cell gets right by re-chasing the slot's CURRENT content every time).
 *
 * Beyond those two cases, skipping the Cell is tempting but UNSAFE for
 * one specific remaining reason, found the hard way (numtide/nix-
 * conformance's eval-okay-eq-slot-elem.nix/-identity.nix regressed,
 * caught by the full test gate): `==` has NO structural equality for
 * functions (always false) UNLESS a slot-IDENTITY shortcut fires first
 * (ll_bshared_slot_identical, builtins_shared.c) -- and a Cell is
 * the ONLY thing that shortcut currently recognizes as "this is
 * deliberately aliasing a shared slot," not "two independently-forced
 * values that happen to read the same bits." Confirmed against real
 * Nix: `let f = x: x; in f == f` is `false` (a direct `==` operand is
 * always a FRESH copy in real Nix too, eval.cc's own ExprOpEq::eval --
 * no identity preserved there either way) but `let f = x: x; in [f] ==
 * [f]` is `true` (each list's own element still points at the exact
 * same slot). Skipping the Cell for an already-populated FUNCTION-typed
 * slot (Closure/PrimOp) would make a list/attrs element behave like a
 * direct operand instead -- collapsing exactly the distinction real Nix
 * (and ll_bshared_slot_identical) relies on. Every OTHER already-
 * populated kind is safe to fast-path: an unforced Thunk pointer is
 * already compared by raw pointer identity (that function's own second
 * branch), and anything else (Attrs/List/String/Path/int/bool) has REAL
 * structural equality as a fallback, so losing identity-trackability
 * there changes nothing observable. */
LL_OP_ATTR Value ll_op_var_ref_cell(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    int64_t depth = ip->a, slot = ip->b;
    Env *e = env;
    for (int64_t d = 0; d < depth; d++) {
        uintptr_t up = e->up & ~(uintptr_t)LL_ENV_WITH_TAG;
        e = (Env *)((uint8_t *)ll_env_arena_base + up);
    }
    Value current = e->values[slot];
    int64_t tag = current.raw & LL_TAG_MASK;
    int fast_path_ok = current.raw != 0 && tag != LL_TAG_CLOSURE && tag != LL_TAG_PRIMOP;
    Value result = fast_path_ok ? current : ll_make_cell_value(&e->values[slot]);
    ll_check_valstack(vs);
    vs[0] = result;
    vs++;
    LL_NEXT(ip, env, vs, pool, rs);
}
