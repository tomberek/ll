/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445's src/asm/ops/op_call.S -- see ~/.claude/plans/soft-wishing-engelbart.md
 *
 * The closure branch is inlined here (RetFrame push + musttail into
 * the callee's own body) rather than routed through ll_apply,
 * specifically so native-stack depth never tracks ordinary Nix-level
 * call depth -- see the M10 plan's own "alternative rejected" note.
 * The __functor branch (an attrset with a __functor attribute is
 * callable, real nixpkgs's lib.fix/lib.extends/overlay machinery uses
 * this pervasively) is a genuine C call, not a tail call -- control
 * needs to come back here to push the result and continue dispatch,
 * matching op_call.S's own .Lop_call_is_attrs (a real `call`, not a
 * tail `jmp`, for exactly that reason). */
#include "ops.h"

#include "builtins_shared.h"

LL_OP_ATTR Value ll_op_call(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    int64_t argc = ip->a;
    Value *callee_slot = vs - argc - 1;
    Value callee = *callee_slot;
    uint64_t tag = callee.raw & LL_TAG_MASK;

    if (tag == LL_TAG_CLOSURE) {
        Closure *c = (Closure *)ll_untag(callee);
        if (c->arity != argc) {
            static const char msg[] = "ll: call argument count does not match arity, aborting\n";
            ll_fatal(msg, sizeof(msg) - 1);
        }
        Env *new_env = ll_env_alloc_fast(c->up, argc);
        for (int64_t i = 0; i < argc; i++) new_env->values[i] = callee_slot[1 + i];

        vs = callee_slot; /* pop callee+args in one shot */

        ll_check_retsp(rs);
        rs[0].ip = ip + 1;
        rs[0].env = env;
        rs[0].pool = pool;
        rs[0].force_target = NULL;
        rs++;

        IrProgram *prog = c->prog;
        env = new_env;
        pool = prog->pool;
        ip = prog->ops;
        [[clang::musttail]] return ((LLOpFn)ip->handler)(ip, env, vs, pool, rs);
    }

    if (tag == LL_TAG_PRIMOP) {
        PrimOp *p = (PrimOp *)ll_untag(callee);
        if (p->arity != argc) {
            static const char msg[] = "ll: call argument count does not match arity, aborting\n";
            ll_fatal(msg, sizeof(msg) - 1);
        }
        Value *args = callee_slot + 1;
        /* M25: force each argument before handing it to the native C
         * function -- primops read args[i]'s raw tag directly and
         * can't force a thunk themselves. Skipped when NOFORCE is set
         * (tryEval's own convention; not otherwise used in this
         * backend's scope). */
        if (!p->noforce) {
            for (int64_t i = 0; i < argc; i++) args[i] = ll_force_for_print(args[i]);
        }
        Value (*fn)(const Value *, int64_t) = (Value(*)(const Value *, int64_t))p->fn;
        Value result = fn(args, argc);
        /* M23: force the primop's raw return value too (a lazy-
         * passthrough primop like getAttr deliberately returns an
         * unforced entry). */
        result = ll_force_for_print(result);

        vs = callee_slot;
        ll_check_valstack(vs);
        vs[0] = result;
        vs++;
        LL_NEXT(ip, env, vs, pool, rs);
    }

    if (tag == LL_TAG_ATTRS && argc == 1) {
        Value result = ll_bshared_call_functor(callee, callee_slot[1]);
        vs = callee_slot;
        ll_check_valstack(vs);
        vs[0] = result;
        vs++;
        LL_NEXT(ip, env, vs, pool, rs);
    }

    {
        static const char msg[] = "ll: attempt to call something which is not a function, aborting\n";
        ll_fatal(msg, sizeof(msg) - 1);
    }
}

/* OP_CALL_CHAIN (ll-only, no asnix equivalent -- see ir.h's own comment):
 * a = n (>= 2), VALSTACK holds [callee, arg0, ..., arg(n-1)] same as an
 * ordinary n-arg OP_CALL would.
 *
 * Phase 1 is a pure, read-only walk -- no allocation, no side effects --
 * checking whether `callee` is the head of a plain one-slot-per-level
 * curry chain at least n-1 levels deep. Every such link (an ordinary
 * non-pattern lambda's own curry stage, AND every curried-native-
 * builtin stage ll_bshared_make_curried_native builds, both use the
 * exact same `ib_lambda(..., 1, ...); ib_ret(...)` shape) compiles to
 * exactly two ops: OP_LAMBDA(arity=1, pidx) then OP_RET, with nothing
 * else -- `level_prog->pool[pidx]` is the next level's own IrProgram*,
 * read directly, without ever constructing the intermediate Closure
 * object that running OP_LAMBDA for real would have built. A level
 * that doesn't match this shape (arity != 1, or its own body is
 * anything other than exactly those two ops -- i.e. real content, the
 * point where the chain's actual work begins) stops the walk.
 *
 * If the walk reaches n-1 links deep, phase 2 builds all n of this
 * call's 1-slot Env frames in ONE bump allocation (same per-frame size
 * ll_env_alloc_fast computes for nslots=1, just n of them back to back,
 * each one's `up` pointing at the previous -- bit-for-bit the same
 * frame CHAIN n separate ll_env_alloc_fast(.., 1) calls would have produced,
 * just one arena bump instead of n), then runs the n-th level's own
 * body (`level_prog` after the walk) with env = the last frame built --
 * identical in every observable way to what n chained ordinary calls
 * would have produced, including a real Closure result when n lands
 * short of the chain's own full depth (partial application: the n-th
 * level's body is then ITSELF exactly `[OP_LAMBDA, OP_RET]`, which
 * runs completely normally, unmodified, and correctly captures the
 * fused frame chain as that new Closure's `up`).
 *
 * Whenever phase 1's walk comes up short (not a Closure, arity != 1 at
 * some level, a level's body isn't that exact two-op shape, or the
 * fused allocation wouldn't fit in the fast env arena) this falls back
 * to n ordinary, sequential single-argument applications via
 * ll_apply -- byte-identical in behavior to the n separate OP_CALL
 * ops this call site would have compiled to before this op existed
 * (ll_apply's own Closure/PrimOp/__functor dispatch is exactly
 * ll_op_call's, just reached by a real C call instead of a tail-jump).
 * Re-entering synchronously here grows the native stack by at most
 * n-1 frames, which is safe: n is bounded by this call site's own
 * SOURCE SYNTAX (how many arguments appear at one application
 * expression in the program text), never by Nix-level, data-dependent
 * recursion depth -- the invariant threaded dispatch exists to
 * protect is about the latter, not a handful of frames bounded by
 * what a human wrote at one call site. */
LL_OP_ATTR Value ll_op_call_chain(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    int64_t n = ip->a;
    Value *callee_slot = vs - n - 1;
    Value callee = *callee_slot;

    if ((callee.raw & LL_TAG_MASK) == LL_TAG_CLOSURE) {
        Closure *c0 = (Closure *)ll_untag(callee);
        if (c0->arity == 1) {
            IrProgram *level_prog = c0->prog;
            int64_t depth;
            for (depth = 0; depth < n - 1; depth++) {
                IrOp *lops = level_prog->ops;
                if ((void *)lops[0].handler != (void *)ll_op_lambda) break;
                if (lops[0].a != 1) break;
                if ((void *)lops[1].handler != (void *)ll_op_ret) break;
                level_prog = (IrProgram *)(uintptr_t)level_prog->pool[lops[0].b];
            }
            if (depth == n - 1) {
                uint64_t one_size = (uint64_t)LL_ENV_OFF_VALUES + 8;
                uint64_t one_n4 = (one_size + 3) & ~(uint64_t)3;
                uint64_t total = one_n4 * (uint64_t)n;
                uint8_t *cur = ll_env_arena_cur;
                uint8_t *new_cur = cur + total;
                if (cur != NULL && new_cur <= ll_env_arena_end) {
                    ll_env_arena_cur = new_cur;
                    ll_stats_env += (uint64_t)n;
                    ll_stats_env_elements += (uint64_t)n;

                    Env *prev = c0->up;
                    uint8_t *frame_mem = cur;
                    for (int64_t i = 0; i < n; i++) {
                        Env *frame = (Env *)frame_mem;
                        frame->up = prev ? (uint32_t)((uintptr_t)prev - (uintptr_t)ll_env_arena_base) : 0;
                        frame->values[0] = callee_slot[1 + i];
                        prev = frame;
                        frame_mem += one_n4;
                    }

                    vs = callee_slot;

                    ll_check_retsp(rs);
                    rs[0].ip = ip + 1;
                    rs[0].env = env;
                    rs[0].pool = pool;
                    rs[0].force_target = NULL;
                    rs++;

                    env = prev;
                    pool = level_prog->pool;
                    ip = level_prog->ops;
                    [[clang::musttail]] return ((LLOpFn)ip->handler)(ip, env, vs, pool, rs);
                }
            }
        }
    }

    Value result = callee;
    for (int64_t i = 0; i < n; i++) {
        /* A NOFORCE PrimOp (e.g. foldl''s own intermediate stages,
         * builtins_list.c) reads its captured values via
         * self_captures(args), which assumes `args[-1]` is the PrimOp
         * Value itself -- an invariant ll_op_call's own direct dispatch
         * gives for free (callee always sits immediately before its
         * args on VALSTACK there), but ll_apply's generic (fn, args,
         * argc) signature does NOT guarantee for an arbitrary caller
         * (it only copies into a safe scratch buffer when the PrimOp
         * forces its args -- see ll_apply_primop_slow). `callee_slot[1+i]`
         * is NOT immediately preceded by `result` on this call site's
         * own VALSTACK (it is preceded by the PREVIOUS argument, or by
         * the original callee), so this step must copy both into a
         * local 2-slot buffer to restore that adjacency, matching
         * exactly what n separate ordinary OP_CALL(1) ops would have
         * had. */
        Value step[2];
        step[0] = result;
        step[1] = callee_slot[1 + i];
        result = ll_apply(step[0], &step[1], 1);
    }
    vs = callee_slot;
    ll_check_valstack(vs);
    vs[0] = result;
    vs++;
    LL_NEXT(ip, env, vs, pool, rs);
}

LL_OP_ATTR Value ll_op_ret(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    (void)ip;
    (void)env;
    (void)pool;
    rs--;
    RetFrame f = *rs;
    if (f.force_target) {
        /* This frame was pushed to force a thunk (OP_VAR_REF), not an
         * ordinary call -- memoize the body's result (already on top
         * of VALSTACK) into the thunk cell before resuming. M82: this
         * thunk is always the current head of ll_inprogress_head
         * (forcing is strictly LIFO) -- pop it (read its "prev" link,
         * stashed in `env` while IN_PROGRESS) before overwriting `env`
         * with the cached result. */
        Thunk *t = (Thunk *)f.force_target;
        ll_inprogress_head = (void *)(uintptr_t)t->env;
        t->env = vs[-1].raw;
        t->state = LL_THUNK_STATE_FORCED;
    }
    ip = f.ip;
    env = f.env;
    pool = f.pool;
    [[clang::musttail]] return ((LLOpFn)ip->handler)(ip, env, vs, pool, rs);
}
