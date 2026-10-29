/* Re-entrant "force a thunk" / "apply a Value" entry points for C code
 * with no active dispatch context on the stack (the value printer, and
 * every map/filter/foldl'-style vendored builtin). Mirrors asnix's own
 * force.S/apply.S sentinel-RetFrame-plus-epilogue trick: mirror the
 * entry prologue (own VALSTACK/RETSP, depth-indexed via
 * force_scratch.c), push one RetFrame whose `ip` points at a tiny
 * native epilogue instead of a real op, musttail into the
 * thunk/closure body; when its own OP_RET resumes that `ip`, it lands
 * on the epilogue, which does a genuine C `return`. */
#include <stdint.h>

#include "op_dispatch.h"
#include "ll/force_scratch.h"
#include "ll/stats.h"
#include "builtins_shared.h"
#include "ll_resolve.h"
#include "tryeval.h"

/* Nested-recursion depth for ll_force_for_print, read/saved/restored
 * by builtins.tryEval (builtins_tryeval.c). */
int64_t ll_force_depth = 0;
int64_t ll_apply_depth = 0;

static LL_OP_ATTR Value ll_force_epilogue(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    (void)ip;
    (void)env;
    (void)pool;
    (void)rs;
    return vs[-1]; /* result, memoized by OP_RET before landing here */
}
static IrOp ll_force_sentinel = {.handler = (void *)ll_force_epilogue, .a = 0, .b = 0};

static LL_OP_ATTR Value ll_apply_epilogue(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    (void)ip;
    (void)env;
    (void)pool;
    (void)rs;
    return vs[-1];
}
static IrOp ll_apply_sentinel = {.handler = (void *)ll_apply_epilogue, .a = 0, .b = 0};

static __attribute__((noinline)) Value ll_force_thunk_slow(Thunk *t, int64_t state_word) {
    /* UNFORCED: state_word IS the clean IrProgram*. */
    IrProgram *prog = (IrProgram *)(uintptr_t)state_word;

    void *saved_vlim = ll_valstack_limit;
    void *saved_rlim = ll_retsp_limit;

    Value *vs = (Value *)ll_force_scratch_valstack(ll_force_depth);
    RetFrame *rs = (RetFrame *)ll_force_scratch_retstack(ll_force_depth);
    ll_valstack_limit = (char *)vs + LL_FORCE_VALSTACK_BYTES;
    ll_retsp_limit = (char *)rs + LL_FORCE_RETSTACK_BYTES;
    ll_force_depth++;

    t->state = LL_THUNK_STATE_IN_PROGRESS;

    rs[0].ip = &ll_force_sentinel;
    rs[0].env = NULL;
    rs[0].pool = NULL;
    rs[0].force_target = t;

    Env *thunk_env = (Env *)(uintptr_t)t->env;
    t->env = (uint64_t)(uintptr_t)ll_inprogress_head;
    ll_inprogress_head = t;

    IrOp *ip0 = prog->ops;
    Value result = ((LLOpFn)ip0->handler)(ip0, thunk_env, vs, prog->pool, rs + 1);

    ll_force_depth--;
    ll_retsp_limit = saved_rlim;
    ll_valstack_limit = saved_vlim;

    return result;
}

/* Value ll_force_for_print(Value v) -- see include/ll/ir.h. Also
 * resolves Cell/LazyApp/ThunkMap indirection (ll_resolve.c) before
 * checking the THUNK tag on each loop iteration -- this is the one
 * force/resolve entry point every vendored builtin and op_select.c's
 * own force-the-selected-entry call goes through, so making it fully
 * general here is what makes every other caller correct for free. */
Value ll_force_for_print(Value v) {
    for (;;) {
        v = ll_resolve_indirection(v);
        if ((v.raw & LL_TAG_MASK) != LL_TAG_THUNK) return v;

        Thunk *t = (Thunk *)ll_untag(v);
        int64_t state_word = t->state;
        int64_t state_tag = state_word & LL_TAG_MASK;

        if (state_tag == LL_THUNK_STATE_FORCED) {
            v.raw = t->env;
            continue; /* a forced result can itself be another unforced
                       * thunk (a lazy-passthrough primop's result) -- loop. */
        }
        if (state_tag == LL_THUNK_STATE_IN_PROGRESS) {
            static const char msg[] = "ll: infinite recursion forcing a thunk (blackhole), aborting\n";
            ll_fatal(msg, sizeof(msg) - 1);
        }
        if (state_tag == LL_THUNK_STATE_FAILED) {
            /* Route through ll_tryeval_raise_or_fatal (tryeval.c), not
             * ll_fatal directly, so re-forcing a thunk whose first
             * evaluation failed inside an active builtins.tryEval
             * re-raises (catchable again) instead of unconditionally
             * aborting the whole process. Falls back to exactly
             * ll_fatal's own write+exit(1) behavior when no tryEval
             * is active. */
            static const char msg[] = "ll: re-evaluation of a thunk whose first evaluation failed, aborting\n";
            ll_tryeval_raise_or_fatal(msg, sizeof(msg) - 1); /* _Noreturn */
        }

        v = ll_force_thunk_slow(t, state_word);
        /* loop: see the FORCED case's own comment above */
    }
}

static __attribute__((noinline)) Value ll_apply_closure_slow(Value fn, const Value *args, int64_t argc) {
    Closure *c = (Closure *)ll_untag(fn);
    if (c->arity != argc) {
        static const char msg[] = "ll: apply argument count does not match arity, aborting\n";
        ll_fatal(msg, sizeof(msg) - 1);
    }

    void *saved_vlim = ll_valstack_limit;
    void *saved_rlim = ll_retsp_limit;

    Value *vs = (Value *)ll_apply_scratch_valstack(ll_apply_depth);
    RetFrame *rs = (RetFrame *)ll_apply_scratch_retstack(ll_apply_depth);
    ll_valstack_limit = (char *)vs + LL_FORCE_VALSTACK_BYTES;
    ll_retsp_limit = (char *)rs + LL_FORCE_RETSTACK_BYTES;
    ll_apply_depth++;

    Env *new_env = ll_env_alloc_fast(c->up, argc);
    for (int64_t i = 0; i < argc; i++) new_env->values[i] = args[i];

    rs[0].ip = &ll_apply_sentinel;
    rs[0].env = NULL;
    rs[0].pool = NULL;
    rs[0].force_target = NULL;

    IrProgram *prog = c->prog;
    IrOp *ip0 = prog->ops;
    Value result = ((LLOpFn)ip0->handler)(ip0, new_env, vs, prog->pool, rs + 1);

    ll_apply_depth--;
    ll_retsp_limit = saved_rlim;
    ll_valstack_limit = saved_vlim;
    return result;
}

static __attribute__((noinline)) Value ll_apply_primop_slow(Value fn, const Value *args, int64_t argc) {
    PrimOp *p = (PrimOp *)ll_untag(fn);
    if (p->arity != argc) {
        static const char msg[] = "ll: apply argument count does not match arity, aborting\n";
        ll_fatal(msg, sizeof(msg) - 1);
    }
    /* Never mutate the caller's own args buffer -- build a local
     * forced copy instead. Primop arities are small; 64 covers any
     * realistic case with room to spare, fixed-size so this stays
     * simple C.
     *
     * scratch[0] holds `fn` itself (the called PrimOp Value), with
     * the forced args starting at scratch+1 -- this is the "self"
     * slot builtins_list.c's self_captures() reads via args[-1],
     * exploiting the real invariant op_call.c's own primop call
     * path gives it for free (the callee's own Value always sits
     * immediately before args[0] on VALSTACK there). ll_apply is
     * a SEPARATE call path from ordinary OP_CALL -- reached when a
     * curried-native stage (e.g. `builtins.elemAt someList`, itself
     * a PrimOp capturing someList) is handed to map/filter/foldl'/
     * genList as the callback -- so it must provide that same
     * invariant itself, not rely on whatever happened to precede
     * the caller's own buffer. */
    Value scratch[65];
    scratch[0] = fn;
    const Value *use_args = args;
    if (!p->noforce) {
        for (int64_t i = 0; i < argc; i++) scratch[1 + i] = ll_force_for_print(args[i]);
        use_args = scratch + 1;
    }
    Value (*fn_ptr)(const Value *, int64_t) = (Value(*)(const Value *, int64_t))p->fn;
    Value result = fn_ptr(use_args, argc);
    return ll_force_for_print(result);
}

Value ll_apply(Value fn, const Value *args, int64_t argc) {
    uint64_t tag = fn.raw & LL_TAG_MASK;

    if (tag == LL_TAG_PRIMOP) {
        return ll_apply_primop_slow(fn, args, argc);
    }

    if (tag == LL_TAG_CLOSURE) {
        return ll_apply_closure_slow(fn, args, argc);
    }

    if (tag == LL_TAG_ATTRS && argc == 1) {
        return ll_bshared_call_functor(fn, args[0]);
    }

    {
        static const char msg[] = "ll: apply target is not a function, aborting\n";
        ll_fatal(msg, sizeof(msg) - 1);
    }
}
