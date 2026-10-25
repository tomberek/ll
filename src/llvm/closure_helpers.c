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
#include "asnix/force_scratch.h"

/* M82: nested-recursion depth for asnix_force_for_print, read/saved/
 * restored by builtins.tryEval (builtins_tryeval.c) -- moved here
 * from backend_stub.c, which this file's rewrite replaces. */
int64_t asnix_force_depth = 0;
int64_t asnix_apply_depth = 0;

static Value ll_force_epilogue(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    (void)ip;
    (void)env;
    (void)pool;
    (void)rs;
    return vs[-1]; /* result, memoized by OP_RET before landing here */
}
static IrOp ll_force_sentinel = {.handler = (void *)ll_force_epilogue, .a = 0, .b = 0};

static Value ll_apply_epilogue(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    (void)ip;
    (void)env;
    (void)pool;
    (void)rs;
    return vs[-1];
}
static IrOp ll_apply_sentinel = {.handler = (void *)ll_apply_epilogue, .a = 0, .b = 0};

/* Value asnix_force_for_print(Value v) -- see include/asnix/ir.h. */
Value asnix_force_for_print(Value v) {
    for (;;) {
        if ((v.raw & ASNIX_TAG_MASK) != ASNIX_TAG_THUNK) return v;

        Thunk *t = (Thunk *)ll_untag(v);
        int64_t state_word = t->state;
        int64_t state_tag = state_word & ASNIX_TAG_MASK;

        if (state_tag == ASNIX_THUNK_STATE_FORCED) {
            v.raw = t->env;
            continue; /* M23: a forced result can itself be another unforced
                       * thunk (a lazy-passthrough primop's result) -- loop. */
        }
        if (state_tag == ASNIX_THUNK_STATE_IN_PROGRESS) {
            static const char msg[] = "asnix: infinite recursion forcing a thunk (blackhole), aborting\n";
            asnix_fatal(msg, sizeof(msg) - 1);
        }
        if (state_tag == ASNIX_THUNK_STATE_FAILED) {
            static const char msg[] = "asnix: re-evaluation of a thunk whose first evaluation failed, aborting\n";
            asnix_fatal(msg, sizeof(msg) - 1);
        }

        /* UNFORCED: state_word IS the clean IrProgram*. */
        IrProgram *prog = (IrProgram *)(uintptr_t)state_word;

        void *saved_vlim = asnix_valstack_limit;
        void *saved_rlim = asnix_retsp_limit;

        Value *vs = (Value *)asnix_force_scratch_valstack(asnix_force_depth);
        RetFrame *rs = (RetFrame *)asnix_force_scratch_retstack(asnix_force_depth);
        asnix_valstack_limit = (char *)vs + ASNIX_FORCE_VALSTACK_BYTES;
        asnix_retsp_limit = (char *)rs + ASNIX_FORCE_RETSTACK_BYTES;
        asnix_force_depth++;

        t->state = ASNIX_THUNK_STATE_IN_PROGRESS;

        rs[0].ip = &ll_force_sentinel;
        rs[0].env = NULL;
        rs[0].pool = NULL;
        rs[0].force_target = t;

        Env *thunk_env = (Env *)(uintptr_t)t->env;
        t->env = (uint64_t)(uintptr_t)asnix_inprogress_head;
        asnix_inprogress_head = t;

        IrOp *ip0 = prog->ops;
        Value result = ((LLOpFn)ip0->handler)(ip0, thunk_env, vs, prog->pool, rs + 1);

        asnix_force_depth--;
        asnix_retsp_limit = saved_rlim;
        asnix_valstack_limit = saved_vlim;

        v = result;
        /* loop: see the FORCED case's own comment above */
    }
}

/* Value asnix_apply(Value fn, const Value *args, int64_t argc) -- see
 * include/asnix/ir.h. Scope matches today's ll: Closure and PrimOp
 * only, no __functor/ATTRS-is-callable protocol. */
Value asnix_apply(Value fn, const Value *args, int64_t argc) {
    uint64_t tag = fn.raw & ASNIX_TAG_MASK;

    if (tag == ASNIX_TAG_PRIMOP) {
        PrimOp *p = (PrimOp *)ll_untag(fn);
        if (p->arity != argc) {
            static const char msg[] = "asnix: apply argument count does not match arity, aborting\n";
            asnix_fatal(msg, sizeof(msg) - 1);
        }
        /* M38: never mutate the caller's own args buffer -- build a
         * local forced copy instead. Primop arities are small (max 3
         * in this codebase today); 64 covers any realistic case with
         * room to spare, fixed-size so this stays simple C. */
        Value forced[64];
        const Value *use_args = args;
        if (!p->noforce) {
            for (int64_t i = 0; i < argc; i++) forced[i] = asnix_force_for_print(args[i]);
            use_args = forced;
        }
        Value (*fn_ptr)(const Value *, int64_t) = (Value(*)(const Value *, int64_t))p->fn;
        Value result = fn_ptr(use_args, argc);
        return asnix_force_for_print(result);
    }

    if (tag == ASNIX_TAG_CLOSURE) {
        Closure *c = (Closure *)ll_untag(fn);
        if (c->arity != argc) {
            static const char msg[] = "asnix: apply argument count does not match arity, aborting\n";
            asnix_fatal(msg, sizeof(msg) - 1);
        }

        void *saved_vlim = asnix_valstack_limit;
        void *saved_rlim = asnix_retsp_limit;

        Value *vs = (Value *)asnix_apply_scratch_valstack(asnix_apply_depth);
        RetFrame *rs = (RetFrame *)asnix_apply_scratch_retstack(asnix_apply_depth);
        asnix_valstack_limit = (char *)vs + ASNIX_FORCE_VALSTACK_BYTES;
        asnix_retsp_limit = (char *)rs + ASNIX_FORCE_RETSTACK_BYTES;
        asnix_apply_depth++;

        Env *new_env = asnix_env_alloc(c->up, argc);
        for (int64_t i = 0; i < argc; i++) new_env->values[i] = args[i];

        rs[0].ip = &ll_apply_sentinel;
        rs[0].env = NULL;
        rs[0].pool = NULL;
        rs[0].force_target = NULL;

        IrProgram *prog = c->prog;
        IrOp *ip0 = prog->ops;
        Value result = ((LLOpFn)ip0->handler)(ip0, new_env, vs, prog->pool, rs + 1);

        asnix_apply_depth--;
        asnix_retsp_limit = saved_rlim;
        asnix_valstack_limit = saved_vlim;
        return result;
    }

    {
        static const char msg[] = "asnix: apply target is not a function, aborting\n";
        asnix_fatal(msg, sizeof(msg) - 1);
    }
}
