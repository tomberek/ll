/* M10: AOT threaded dispatch -- the handler signature and VM-state
 * shape every op handler (src/llvm/ops) and the dispatch-table
 * linker (dispatch_table.c) share. Mirrors asnix's own dispatch.S
 * register convention (%rsi=IP, %rdi=ENV, %r12=VALSTACK, %r13=POOL,
 * %r14=RETSP) as five plain C arguments instead -- see
 * ~/.claude/plans/soft-wishing-engelbart.md's M10 section for the full
 * design rationale (musttail-guaranteed tail calls in place of asm's
 * `jmp *%rax`). */
#ifndef LL_OP_DISPATCH_H
#define LL_OP_DISPATCH_H

#include <stddef.h>
#include <stdint.h>

#include "asnix/arena.h"
#include "asnix/env.h"
#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/value.h"

/* Call-frame record on the explicit RETSP stack -- same shape/offsets
 * as asnix's own RetFrame (ir.h's ASNIX_RETFRAME_OFF_* constants), just
 * expressed as a real C struct instead of raw offsets into asm. */
typedef struct {
    IrOp *ip;
    Env *env;
    int64_t *pool;
    void *force_target; /* Thunk*, or NULL for an ordinary call */
} RetFrame;

_Static_assert(offsetof(RetFrame, ip) == ASNIX_RETFRAME_OFF_IP, "RetFrame.ip offset");
_Static_assert(offsetof(RetFrame, env) == ASNIX_RETFRAME_OFF_ENV, "RetFrame.env offset");
_Static_assert(offsetof(RetFrame, pool) == ASNIX_RETFRAME_OFF_POOL, "RetFrame.pool offset");
_Static_assert(offsetof(RetFrame, force_target) == ASNIX_RETFRAME_OFF_FORCE_TARGET, "RetFrame.force_target offset");
_Static_assert(sizeof(RetFrame) == ASNIX_RETFRAME_SIZE, "RetFrame size");

/* Closure heap object -- same shape as asnix's own (ir.h's
 * ASNIX_CLOSURE_OFF_* constants): {up; prog; arity; formals}. */
typedef struct {
    Env *up;
    IrProgram *prog;
    int64_t arity;
    Value formals;
} Closure;

_Static_assert(offsetof(Closure, up) == ASNIX_CLOSURE_OFF_UP, "Closure.up offset");
_Static_assert(offsetof(Closure, prog) == ASNIX_CLOSURE_OFF_PROG, "Closure.prog offset");
_Static_assert(offsetof(Closure, arity) == ASNIX_CLOSURE_OFF_ARITY, "Closure.arity offset");
_Static_assert(offsetof(Closure, formals) == ASNIX_CLOSURE_OFF_FORMALS, "Closure.formals offset");
_Static_assert(sizeof(Closure) == ASNIX_CLOSURE_SIZE, "Closure size");

/* Thunk heap object -- same tag-packed shape as asnix's own (ir.h's
 * ASNIX_THUNK_OFF_* comment): `state` IS the clean IrProgram* while
 * UNFORCED (tag bits 0), or a bare state tag (1/2/3) otherwise; `env`
 * is the captured Env* while UNFORCED, the intrusive in-progress
 * "prev" link while IN_PROGRESS, or the cached Value.raw while FORCED. */
typedef struct {
    int64_t state;
    uint64_t env;
} Thunk;

_Static_assert(offsetof(Thunk, state) == ASNIX_THUNK_OFF_STATE, "Thunk.state offset");
_Static_assert(offsetof(Thunk, env) == ASNIX_THUNK_OFF_ENV, "Thunk.env offset");
_Static_assert(sizeof(Thunk) == ASNIX_THUNK_SIZE, "Thunk size");

/* PrimOp heap object -- same shape as asnix's own (heap.h's
 * ASNIX_PRIMOP_OFF_* constants): {fn; arity; noforce}. */
typedef struct {
    void *fn; /* Value (*)(const Value *args, int64_t argc) */
    int64_t arity;
    int64_t noforce;
} PrimOp;

_Static_assert(offsetof(PrimOp, fn) == ASNIX_PRIMOP_OFF_FN, "PrimOp.fn offset");
_Static_assert(offsetof(PrimOp, arity) == ASNIX_PRIMOP_OFF_ARITY, "PrimOp.arity offset");
_Static_assert(offsetof(PrimOp, noforce) == ASNIX_PRIMOP_OFF_NOFORCE, "PrimOp.noforce offset");
_Static_assert(sizeof(PrimOp) == ASNIX_PRIMOP_SIZE, "PrimOp size");

/* Every op handler has exactly this signature -- a hard requirement
 * for musttail (the callee's type must match the caller's). */
typedef Value (*LLOpFn)(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);

static inline void *ll_untag(Value v) {
    return (void *)(uintptr_t)(v.raw & ~(uint64_t)ASNIX_TAG_MASK);
}

static inline Value ll_true(void) {
    Value v;
    v.raw = ((uint64_t)ASNIX_IMM_TRUE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM;
    return v;
}

static inline Value ll_false(void) {
    Value v;
    v.raw = ((uint64_t)ASNIX_IMM_FALSE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM;
    return v;
}

/* stacklimits.S's globals/routines (asm-only today, no C header
 * declares them) -- vendored unchanged, used directly from C here. */
extern void *asnix_valstack_limit;
extern void *asnix_retsp_limit;
extern void *asnix_inprogress_head;
extern void asnix_valstack_overflow(void) __attribute__((noreturn));
extern void asnix_retsp_overflow(void) __attribute__((noreturn));
extern void asnix_fatal(const char *msg, uint64_t len) __attribute__((noreturn));

static inline void ll_check_valstack(Value *vs) {
    if ((void *)(vs + 1) > asnix_valstack_limit) asnix_valstack_overflow();
}

static inline void ll_check_retsp(RetFrame *rs) {
    if ((void *)(rs + 1) > asnix_retsp_limit) asnix_retsp_overflow();
}

/* Guaranteed-tail-call dispatch to the next op, advancing IP by one
 * record first -- the direct C equivalent of dispatch.S's universal
 * NEXT tail ("IP += 16; jmp *(IP->handler)"). Must be the LAST
 * statement in a handler (expands to two statements, not an
 * expression) -- never use inside an unbraced if/else arm. Only for
 * handlers that have NOT already computed their own next `ip`
 * (anything that branches -- OP_IF/OP_JMP/the fused _IF ops -- must
 * use LL_GOTO below instead, or this double-advances). */
#define LL_NEXT(ip, env, vs, pool, rs) \
    (ip)++; \
    [[clang::musttail]] return ((LLOpFn)(ip)->handler)((ip), (env), (vs), (pool), (rs))

/* Guaranteed-tail-call dispatch to whatever `ip` already points at --
 * for handlers that computed their own next IP (a fallthrough ip+1,
 * or a self-relative byte delta). Does NOT advance ip itself. */
#define LL_GOTO(ip, env, vs, pool, rs) \
    [[clang::musttail]] return ((LLOpFn)(ip)->handler)((ip), (env), (vs), (pool), (rs))

extern void *ll_handler_table[OP_COUNT];

#endif /* LL_OP_DISPATCH_H */
