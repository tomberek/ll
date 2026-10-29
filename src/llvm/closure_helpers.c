/* M10: re-entrant "force a thunk" / "apply a Value" entry points for
 * C code with no active dispatch context on the stack (the value
 * printer, and every map/filter/foldl'-style vendored builtin). Ports
 * asnix's own force.S/apply.S sentinel-RetFrame-plus-epilogue trick:
 * mirror the entry prologue (own VALSTACK/RETSP, depth-indexed via
 * the already-vendored force_scratch.c), push one RetFrame whose `ip`
 * points at a tiny native epilogue instead of a real op, musttail
 * into the thunk/closure body; when its own OP_RET resumes that `ip`,
 * it lands on the epilogue, which does a genuine C `return`.
 *
 * Replaces the earlier per-invocation-JIT design, which stored a
 * native function pointer per closure/thunk body (since each body
 * was its own freshly JIT-compiled LLVM function) and could call it
 * directly with no re-entry machinery needed at all. Now that bodies
 * are IrProgram*s dispatched through the shared op-handler table
 * again, closures/thunks go back to asnix's OWN real heap layout
 * (op_dispatch.h's Closure/Thunk), and this file needs the full
 * mechanism asnix's own force.S/apply.S already worked out. */
#include <stdint.h>

#include "op_dispatch.h"
#include "ll/force_scratch.h"
#include "ll/stats.h"
#include "builtins_shared.h"
#include "ll_resolve.h"
#include "tryeval.h"

/* M82: nested-recursion depth for ll_force_for_print, read/saved/
 * restored by builtins.tryEval (builtins_tryeval.c) -- moved here
 * from backend_stub.c, which this file's rewrite replaces. */
int64_t ll_force_depth = 0;
int64_t ll_apply_depth = 0;

/* M10 ll port: ll_apply's Closure branch makes one real, non-tail
 * call to ll_env_alloc per invocation (env.S) -- unlike the op
 * handlers in src/llvm/ops (op_frame.c/op_call.c/op_with.c, see those
 * files' own inlined-env-arena-fast-path edits, applied separately),
 * ll_apply is not a preserve_none handler, so this isn't about
 * register-spill pressure; it's just the ordinary cost of a real call
 * (push/pop + call/ret) versus a few inlined instructions for the
 * overwhelmingly common case. Reuses op_dispatch.h's own ll_env_alloc_fast
 * (already landed there for op_frame.c/op_call.c/op_with.c's identical
 * need) instead of keeping a second, near-duplicate inline copy here --
 * the two were originally developed in parallel, isolated worktrees
 * that couldn't see each other's edits, and merged into master without
 * deduplicating; now that both live in the same file's include graph
 * (closure_helpers.c already #includes "op_dispatch.h"), there's no
 * reason for ll_apply not to call the shared one directly. */
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

/* M10 ll port: pulled out of ll_force_for_print's loop body (see
 * that function's own comment) purely so the UNFORCED/slow path's own
 * VALSTACK/RETSP setup -- which needs several callee-saved registers
 * live across its two ll_force_scratch_* calls -- doesn't force the
 * COMMON "already forced, or not even a thunk" case to pay for an
 * unconditional push-every-callee-saved-register prologue too.
 * Disassembly comparison against asnix's own hand-written force.S
 * (`objdump -d --disassemble=ll_force_for_print`) showed exactly
 * this gap: asnix's asm only pushes %r12/%r13/%r14/%rbx/%rbp/%r15 on
 * the branch that actually confirms an UNFORCED Thunk, while the
 * compiled C version (before this split) pushed all six at function
 * entry unconditionally -- paid even by a call that immediately
 * returns a plain int/string. Pure code motion, no logic change: same
 * statements, same order, just split out of the loop into their own
 * `noinline` function so clang can shrink-wrap the outer loop's
 * prologue around the (rare) call to this one instead of the (common)
 * early return above it. */
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

/* Value ll_force_for_print(Value v) -- see include/ll/ir.h.
 * Also resolves Cell/LazyApp/ThunkMap indirection (ll_resolve.c)
 * before checking the THUNK tag on each loop iteration, matching
 * asnix's own force.S (which handles all of these, not just Thunk --
 * see that file's .Lforce_check_cell/_lazyapp/_thunkmap labels). This
 * is the one force/resolve entry point every vendored builtin and
 * op_select.c's own force-the-selected-entry call already goes
 * through, so making it fully general here is what makes every other
 * caller correct for free. */
Value ll_force_for_print(Value v) {
    for (;;) {
        v = ll_resolve_indirection(v);
        if ((v.raw & LL_TAG_MASK) != LL_TAG_THUNK) return v;

        Thunk *t = (Thunk *)ll_untag(v);
        int64_t state_word = t->state;
        int64_t state_tag = state_word & LL_TAG_MASK;

        if (state_tag == LL_THUNK_STATE_FORCED) {
            v.raw = t->env;
            continue; /* M23: a forced result can itself be another unforced
                       * thunk (a lazy-passthrough primop's result) -- loop. */
        }
        if (state_tag == LL_THUNK_STATE_IN_PROGRESS) {
            static const char msg[] = "ll: infinite recursion forcing a thunk (blackhole), aborting\n";
            ll_fatal(msg, sizeof(msg) - 1);
        }
        if (state_tag == LL_THUNK_STATE_FAILED) {
            /* M82: matches force.S's own .Lforce_reraise -- route
             * through ll_tryeval_raise_or_fatal (tryeval.c), not
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

/* M10 ll port, same spirit as ll_force_thunk_slow's own split above:
 * ll_apply's Closure branch is the only one of its three live
 * branches (PrimOp/Closure/Attrs-functor) that needs a real nested
 * dispatch context (own VALSTACK/RETSP, env allocation, a sentinel
 * RetFrame) -- asnix's own hand-written apply.S reflects that
 * directly: the PrimOp branch spills its small forced-args copy to a
 * plain stack scratch buffer (no callee-saved registers survive its
 * calls), and the Attrs/__functor branch is a bare tail-jmp with NO
 * prologue at all, while ONLY the Closure branch (apply.S's own
 * .Lapply_is_closure) pushes all six callee-saved registers. Before
 * this split, compiling all three branches into ONE function made
 * clang pick the UNION of every branch's register needs for a single
 * shared prologue/epilogue -- paid even by a plain primop call (e.g.
 * `builtins.elemAt someList` invoked through `map`) or the attrset
 * __functor path, neither of which asnix's own asm burdens this way
 * (confirmed via `objdump -d --disassemble=ll_apply`: before this
 * split, every path through the compiled function pushed/popped all
 * six of %rbp/%r15/%r14/%r13/%r12/%rbx, where asnix's own asm only
 * does that on .Lapply_is_closure). Pure code motion, no logic
 * change -- same statements, same order, just split out of the
 * branch into its own `noinline` function so clang can shrink-wrap
 * the outer function's prologue around the (comparatively rare) real
 * Closure call instead of every path through ll_apply. */
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

/* M10 ll port, same split as ll_apply_closure_slow just above: the
 * PrimOp branch's own forced-args copy loop calls ll_force_for_print
 * (and the primop's own fn pointer) several times, so clang must keep
 * fn/args/argc alive in callee-saved registers across those calls --
 * real register pressure, but PrimOp-only. Left inline in ll_apply
 * (pre-split), this pressure leaked into the Attrs/__functor branch and
 * the type/arity error branches too (none of which make any call that
 * needs a register to survive it -- asnix's own apply.S reflects this:
 * .Lapply_is_attrs is a bare tail-jmp with no prologue at all), because
 * one compiled function pays the UNION of every branch's register
 * needs for a single shared prologue/epilogue. Pure code motion, no
 * logic change -- same statements, same order, just split out so
 * clang can shrink-wrap ll_apply's own prologue around only the
 * branches that actually need it. */
static __attribute__((noinline)) Value ll_apply_primop_slow(Value fn, const Value *args, int64_t argc) {
    PrimOp *p = (PrimOp *)ll_untag(fn);
    if (p->arity != argc) {
        static const char msg[] = "ll: apply argument count does not match arity, aborting\n";
        ll_fatal(msg, sizeof(msg) - 1);
    }
    /* M38/M62: never mutate the caller's own args buffer -- build a
     * local forced copy instead. Primop arities are small (max 3
     * in this codebase today); 64 covers any realistic case with
     * room to spare, fixed-size so this stays simple C.
     *
     * scratch[0] holds `fn` itself (the called PrimOp Value), with
     * the forced args starting at scratch+1 -- this is the "self"
     * slot builtins_list.c's self_captures() reads via args[-1],
     * exploiting the real invariant op_call.S's own primop call
     * path gives it for free (the callee's own Value always sits
     * immediately before args[0] on VALSTACK there). ll_apply is
     * a SEPARATE call path from ordinary OP_CALL -- reached when a
     * curried-native stage (e.g. `builtins.elemAt someList`, itself
     * a PrimOp capturing someList) is handed to map/filter/foldl'/
     * genList as the callback -- so it must provide that same
     * invariant itself, not rely on whatever happened to precede
     * the caller's own buffer (which is often a bare local variable
     * with nothing meaningful before it, e.g. genList2_impl's `&iv`). */
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

/* Value ll_apply(Value fn, const Value *args, int64_t argc) -- see
 * include/ll/ir.h. Scope matches today's ll: Closure and PrimOp
 * only, no __functor/ATTRS-is-callable protocol. */
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
