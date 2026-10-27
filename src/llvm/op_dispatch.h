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

/* Every op handler is declared and defined with this attribute --
 * `preserve_none`, first available in clang 19 (see flake.nix's own
 * comment on why), tells the compiler these five arguments are the
 * ENTIRE live state threaded through dispatch; nothing else needs to
 * survive a hop. Concretely, this lets the compiler assign vs/pool/rs
 * to registers that are callee-saved under the PLAIN C functions this
 * code calls out to (asnix_arena_alloc, asnix_force_for_print, ...),
 * so a real (non-tail) call mid-handler no longer needs to spill and
 * reload all five -- the same property asnix's own hand-written asm
 * gets by manually pinning its five dispatch registers across the
 * ABI's callee-saved set. A hard requirement for musttail regardless
 * (the callee's type, attribute included, must match the caller's). */
#define LL_OP_ATTR __attribute__((preserve_none))

typedef LL_OP_ATTR Value (*LLOpFn)(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs);

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

/* Inlined fast path for arena.S's own bump allocator (same rounding,
 * same cap check, same field layout -- see arena.h's own comment on
 * why the C struct and the asm share field offsets). Every op handler
 * that allocates (OP_LAMBDA, OP_MK_THUNK, OP_MK_ATTRS, ...) is one of
 * this file's five-argument LLOpFn handlers, and the SysV ABI puts all
 * five of ip/env/vs/pool/rs in CALLER-saved registers (rdi/rsi/rdx/
 * rcx/r8) -- unlike asnix's own hand-written asm, which deliberately
 * pins vs/pool/rs in CALLEE-saved registers (%r12/%r13/%r14) so a real
 * `call asnix_arena_alloc` only has to spill the two caller-saved ones
 * it actually uses (ip, env). A plain `call asnix_arena_alloc` from a C
 * handler has no such luck: the compiler must spill and reload all
 * five around the call (confirmed via `objdump -d`: 7 register pushes
 * in ll_op_mk_thunk's compiled code vs 2 in asnix_op_mk_thunk's hand-
 * written asm). Inlining the bump-pointer fast path here removes the
 * call (and thus the spills) from the overwhelmingly common case;
 * growth/cap-exceeded still falls back to the real, out-of-line
 * asnix_arena_alloc, unchanged. */
static inline void *ll_arena_alloc(uint64_t n) {
    uint64_t n8 = (n + 7) & ~(uint64_t)7;
    if (asnix_arena.max_memory != 0 && asnix_arena.total_allocated + n8 > asnix_arena.max_memory) {
        return asnix_arena_alloc(n);
    }
    uint8_t *cur = asnix_arena.cur;
    uint8_t *new_cur = cur + n8;
    if (new_cur > asnix_arena.end) {
        return asnix_arena_alloc(n);
    }
    asnix_arena.cur = new_cur;
    asnix_arena.total_allocated += n8;
    return cur;
}

/* env_arena.S's own bump-pointer state -- asm-only today (like
 * stacklimits.S's globals above), no C header declares cur/end
 * (env.h only exposes asnix_env_arena_base, needed by every up-
 * decompression site across the codebase); asnix_stats_env/_elements
 * DO already have extern declarations (stats.h), but are re-declared
 * here too rather than pulling in a whole new include for two counters,
 * matching how this file prefers local, narrowly-scoped externs (see
 * stacklimits.S's globals just above). */
extern uint8_t *asnix_env_arena_cur;
extern uint8_t *asnix_env_arena_end;
extern uint64_t asnix_stats_env;
extern uint64_t asnix_stats_env_elements;

/* Inlined fast path for env_arena.S's own bump allocator -- the
 * SEPARATE, dedicated, bounded Env arena (env_arena.S/env.h's own
 * comment on why Env gets its own arena instead of sharing the general
 * asnix_arena ll_arena_alloc above wraps), mirroring asnix_env_alloc's
 * (env.S) logic exactly: size = ASNIX_ENV_OFF_VALUES + nslots*8, rounded
 * up to 4 (NOT 8 -- Env is never a tagged Value, env.h/env_arena.S's own
 * comments), bump-allocate from asnix_env_arena_cur/_end, bump
 * asnix_stats_env/_elements, then compress (NULL-safe) and write `up`
 * into the new Env's ASNIX_ENV_OFF_UP field. OP_FRAME_ALLOC (op_frame.c),
 * OP_CALL's closure branch (op_call.c), and OP_WITH (op_with.c) all call
 * this as a real, non-tail call mid-handler -- same register-spill
 * problem ll_arena_alloc above fixes for the general arena (confirmed
 * via `objdump -d --disassemble=ll_op_frame_alloc`). The slow path
 * (arena not yet lazily initialized -- cur==0 -- or this allocation
 * doesn't fit before `end`) falls back to the real, unchanged
 * asnix_env_alloc, so the one-time lazy init (mmap'ing the arena) and
 * the hard ASNIX_ENV_ARENA_SIZE cap-exceeded abort stay centralized
 * there exactly as before. */
static inline Env *ll_env_alloc(Env *up, int64_t nslots) {
    uint8_t *cur = asnix_env_arena_cur;
    if (cur == NULL) {
        return asnix_env_alloc(up, nslots);
    }
    uint64_t size = (uint64_t)ASNIX_ENV_OFF_VALUES + (uint64_t)nslots * 8;
    uint64_t n4 = (size + 3) & ~(uint64_t)3;
    uint8_t *new_cur = cur + n4;
    if (new_cur > asnix_env_arena_end) {
        return asnix_env_alloc(up, nslots);
    }
    asnix_env_arena_cur = new_cur;
    asnix_stats_env++;
    asnix_stats_env_elements += (uint64_t)nslots;
    Env *e = (Env *)cur;
    e->up = up ? (uint32_t)((uintptr_t)up - (uintptr_t)asnix_env_arena_base) : 0;
    return e;
}

/* Inlined fast path for env.S's own asnix_env_lookup (De Bruijn
 * depth/slot walk) -- env.S's OWN header comment says the hot
 * dispatch path is supposed to "inline the same logic directly...
 * rather than paying a `call` per use" (same precedent as this
 * file's own ll_env_alloc above, and matching asnix's own
 * op_var_ref.S, which inlines this exact walk and never calls
 * asnix_env_lookup at all) -- op_var_ref.c's two handlers previously
 * called the real, non-tail asnix_env_lookup on every variable
 * reference instead, missing that precedent. Decompress `up` (base +
 * offset), then mask off ASNIX_ENV_WITH_TAG from the DECOMPRESSED
 * POINTER (not from `up` itself) -- same order asnix_env_lookup/
 * op_var_ref.S both use, needed so a hop that happens to pass through
 * a with-frame on its way to an outer lexical binding still lands on
 * the right address (env.h's own ASNIX_ENV_WITH_TAG comment). */
static inline Value ll_env_lookup(Env *env, int64_t depth, int64_t slot) {
    for (; depth > 0; depth--) {
        uintptr_t decompressed = (uintptr_t)asnix_env_arena_base + env->up;
        env = (Env *)(decompressed & ~(uintptr_t)ASNIX_ENV_WITH_TAG);
    }
    return env->values[slot];
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
