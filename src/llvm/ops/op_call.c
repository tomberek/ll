/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445's src/asm/ops/op_call.S -- see ~/.claude/plans/soft-wishing-engelbart.md
 *
 * Scope matches today's ll: Closure and PrimOp callees only, no
 * `__functor`/ATTRS-is-callable protocol (closure_helpers.c's
 * asnix_apply has never implemented that branch either). The closure
 * branch is inlined here (RetFrame push + musttail into the callee's
 * own body) rather than routed through asnix_apply, specifically so
 * native-stack depth never tracks ordinary Nix-level call depth -- see
 * the M10 plan's own "alternative rejected" note. */
#include "ops.h"

Value ll_op_call(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    int64_t argc = ip->a;
    Value *callee_slot = vs - argc - 1;
    Value callee = *callee_slot;
    uint64_t tag = callee.raw & ASNIX_TAG_MASK;

    if (tag == ASNIX_TAG_CLOSURE) {
        Closure *c = (Closure *)ll_untag(callee);
        if (c->arity != argc) {
            static const char msg[] = "asnix: call argument count does not match arity, aborting\n";
            asnix_fatal(msg, sizeof(msg) - 1);
        }
        Env *new_env = asnix_env_alloc(c->up, argc);
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

    if (tag == ASNIX_TAG_PRIMOP) {
        PrimOp *p = (PrimOp *)ll_untag(callee);
        if (p->arity != argc) {
            static const char msg[] = "asnix: call argument count does not match arity, aborting\n";
            asnix_fatal(msg, sizeof(msg) - 1);
        }
        Value *args = callee_slot + 1;
        /* M25: force each argument before handing it to the native C
         * function -- primops read args[i]'s raw tag directly and
         * can't force a thunk themselves. Skipped when NOFORCE is set
         * (tryEval's own convention; not otherwise used in this
         * backend's scope). */
        if (!p->noforce) {
            for (int64_t i = 0; i < argc; i++) args[i] = asnix_force_for_print(args[i]);
        }
        Value (*fn)(const Value *, int64_t) = (Value(*)(const Value *, int64_t))p->fn;
        Value result = fn(args, argc);
        /* M23: force the primop's raw return value too (a lazy-
         * passthrough primop like getAttr deliberately returns an
         * unforced entry). */
        result = asnix_force_for_print(result);

        vs = callee_slot;
        ll_check_valstack(vs);
        vs[0] = result;
        vs++;
        LL_NEXT(ip, env, vs, pool, rs);
    }

    {
        static const char msg[] = "asnix: attempt to call something which is not a function, aborting\n";
        asnix_fatal(msg, sizeof(msg) - 1);
    }
}

Value ll_op_ret(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    (void)ip;
    (void)env;
    (void)pool;
    rs--;
    RetFrame f = *rs;
    if (f.force_target) {
        /* This frame was pushed to force a thunk (OP_VAR_REF), not an
         * ordinary call -- memoize the body's result (already on top
         * of VALSTACK) into the thunk cell before resuming. M82: this
         * thunk is always the current head of asnix_inprogress_head
         * (forcing is strictly LIFO) -- pop it (read its "prev" link,
         * stashed in `env` while IN_PROGRESS) before overwriting `env`
         * with the cached result. */
        Thunk *t = (Thunk *)f.force_target;
        asnix_inprogress_head = (void *)(uintptr_t)t->env;
        t->env = vs[-1].raw;
        t->state = ASNIX_THUNK_STATE_FORCED;
    }
    ip = f.ip;
    env = f.env;
    pool = f.pool;
    [[clang::musttail]] return ((LLOpFn)ip->handler)(ip, env, vs, pool, rs);
}
