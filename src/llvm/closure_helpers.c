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
#include "asnix/stats.h"
#include "builtins_shared.h"
#include "ll_resolve.h"

/* M82: nested-recursion depth for asnix_force_for_print, read/saved/
 * restored by builtins.tryEval (builtins_tryeval.c) -- moved here
 * from backend_stub.c, which this file's rewrite replaces. */
int64_t asnix_force_depth = 0;
int64_t asnix_apply_depth = 0;

/* M10 ll port: asnix_apply's Closure branch makes one real, non-tail
 * call to asnix_env_alloc per invocation (env.S) -- unlike the op
 * handlers in src/llvm/ops (op_frame.c/op_call.c/op_with.c, see those
 * files' own inlined-env-arena-fast-path edits, applied separately),
 * asnix_apply is not a preserve_none handler, so this isn't about
 * register-spill pressure; it's just the ordinary cost of a real call
 * (push/pop + call/ret) versus a few inlined instructions for the
 * overwhelmingly common case. Same trick as op_dispatch.h's own
 * ll_arena_alloc (already landed for the general asnix_arena): inline
 * the bump-allocator fast path, fall back to the real out-of-line
 * asnix_env_alloc only when the arena isn't initialized yet or this
 * allocation would overflow it. asnix_env_arena_cur/_end are normally
 * file-local to src/asm/env_arena.S -- exported (see that file) purely
 * so this static inline can read/bump them directly without a call,
 * mirroring asnix_env_alloc's own env.S logic: size =
 * ASNIX_ENV_OFF_VALUES + nslots*8, rounded up to a multiple of 4 (this
 * fast path bypasses asnix_env_arena_alloc entirely, so it redoes that
 * same round_up_4 itself -- see env_arena.S's own header comment for
 * why 4, not 8), plus the NULL-safe `up` compression asnix_env_alloc's
 * own general-purpose entry point does (unlike the hot-path op
 * handlers, which only ever see a non-NULL current ENV). */
extern void *asnix_env_arena_cur;
extern void *asnix_env_arena_end;

static inline Env *ll_env_alloc_fast(Env *up, int64_t nslots) {
    uint8_t *cur = (uint8_t *)asnix_env_arena_cur;
    if (cur == NULL) return asnix_env_alloc(up, nslots); /* arena not yet initialized */

    uint64_t n = (uint64_t)ASNIX_ENV_OFF_VALUES + (uint64_t)nslots * 8;
    uint64_t n4 = (n + 3) & ~(uint64_t)3;
    uint8_t *new_cur = cur + n4;
    if (new_cur > (uint8_t *)asnix_env_arena_end) return asnix_env_alloc(up, nslots);

    asnix_env_arena_cur = new_cur;
    asnix_stats_env++;
    asnix_stats_env_elements += (uint64_t)nslots;

    Env *e = (Env *)cur;
    e->up = up ? (uint32_t)((uintptr_t)up - (uintptr_t)asnix_env_arena_base) : 0;
    return e;
}

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

/* M10 ll port: pulled out of asnix_force_for_print's loop body (see
 * that function's own comment) purely so the UNFORCED/slow path's own
 * VALSTACK/RETSP setup -- which needs several callee-saved registers
 * live across its two asnix_force_scratch_* calls -- doesn't force the
 * COMMON "already forced, or not even a thunk" case to pay for an
 * unconditional push-every-callee-saved-register prologue too.
 * Disassembly comparison against asnix's own hand-written force.S
 * (`objdump -d --disassemble=asnix_force_for_print`) showed exactly
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

    return result;
}

/* Value asnix_force_for_print(Value v) -- see include/asnix/ir.h.
 * Also resolves Cell/LazyApp/ThunkMap indirection (ll_resolve.c)
 * before checking the THUNK tag on each loop iteration, matching
 * asnix's own force.S (which handles all of these, not just Thunk --
 * see that file's .Lforce_check_cell/_lazyapp/_thunkmap labels). This
 * is the one force/resolve entry point every vendored builtin and
 * op_select.c's own force-the-selected-entry call already goes
 * through, so making it fully general here is what makes every other
 * caller correct for free. */
Value asnix_force_for_print(Value v) {
    for (;;) {
        v = ll_resolve_indirection(v);
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

        v = ll_force_thunk_slow(t, state_word);
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
         * immediately before args[0] on VALSTACK there). asnix_apply is
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
            for (int64_t i = 0; i < argc; i++) scratch[1 + i] = asnix_force_for_print(args[i]);
            use_args = scratch + 1;
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

        Env *new_env = ll_env_alloc_fast(c->up, argc);
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

    if (tag == ASNIX_TAG_ATTRS && argc == 1) {
        return asnix_bshared_call_functor(fn, args[0]);
    }

    {
        static const char msg[] = "asnix: apply target is not a function, aborting\n";
        asnix_fatal(msg, sizeof(msg) - 1);
    }
}
