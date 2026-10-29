/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_ENV_H
#define LL_ENV_H

/* Env{up; values[]}: a flat per-scope frame, chained via `up` for lexical
 * lookup (De Bruijn depth,slot resolved at compile time). One frame per
 * let/lambda-call, matching how cppnix/iets structure Env.
 *
 * M118: `up` is a 32-bit offset into the dedicated, bounded
 * `ll_env_arena` (env_arena.S), not a raw 64-bit pointer -- see that
 * file's header comment for the reservation mechanism and docs/
 * architecture.md's M118 section for why Env specifically (unlike
 * String/Attrs/List/Closure/Thunk/PrimOp, which all stay in the
 * general, unbounded `ll_arena`) can use a DIFFERENT, tighter arena:
 * Env is never itself a tagged Value, so it doesn't need to honor
 * Value.raw's 8-byte/3-tag-bit alignment invariant the general arena's
 * `round_up_8` exists to guarantee for everything else. 0 always means
 * NULL -- the dedicated arena's bump cursor deliberately starts at
 * base+8, never base+0, so a real allocation can never collide with
 * the NULL sentinel. This drops LL_ENV_OFF_VALUES from 8 to 4 --
 * every access site that uses this macro (every one in this codebase
 * does; none hardcode a bare `8`) picks the new offset up automatically. */
#define LL_ENV_OFF_UP     0
#define LL_ENV_OFF_VALUES 4

/* `with`: a with-frame is an ordinary 1-slot Env{up;values[0]=attrs} with
 * one difference -- its OWN `up` field has this tag bit OR'd in. M118:
 * this still works on the COMPRESSED 32-bit offset exactly as it did on
 * the full pointer: the offset is (real_addr - ll_env_arena_base),
 * and since the arena's base (page-aligned, from mmap) and every real
 * Env address within it (round_up_4, env_arena.S) are both at least
 * 4-byte aligned, the low bit of that subtraction is preserved exactly
 * the same way the low bits of "full pointer minus a 0 base" always
 * were -- so the tag bit round-trips through compress/decompress
 * cleanly. Every walk that blindly chases `up` across a *generic* hop
 * (not specifically looking for a with-frame -- e.g. OP_VAR_REF's depth
 * walk, which may pass straight through a with-frame on its way to an
 * outer lexical binding) must mask this bit back off AFTER
 * decompressing, since nothing else ever sets it. */
#define LL_ENV_WITH_TAG   1

/* M118: dedicated, bounded Env arena size -- see env_arena.S. 1 GiB
 * gives ~36x headroom over the ~1.49M envs (~28 MB raw, before this
 * change) measured on this session's real ffmpeg-closure benchmark
 * (`./asnix d.nix`, LL_SHOW_STATS=1); a hard abort (ll_fatal,
 * same philosophy as the general arena's own LL_MAX_MEMORY cap) if
 * a program's live Env count ever actually reaches it -- this is a new
 * permanent constraint the general arena does not otherwise impose
 * (its own cap defaults to unlimited). Must stay well under 4 GiB
 * (2^32) so every offset this arena ever hands out always fits in the
 * 32-bit field above with room to spare. */
#define LL_ENV_ARENA_SIZE 1073741824

#ifndef __ASSEMBLER__

#include <stdint.h>
#include "ll/value.h"

/* M118: `up` is a compressed 32-bit offset (see above), not
 * `struct Env *up` -- packed so `values[]` sits at byte offset 4,
 * matching LL_ENV_OFF_VALUES exactly (an un-packed struct would
 * insert 4 bytes of alignment padding before `values[0]` to keep
 * `Value` 8-aligned, silently defeating the whole point of shrinking
 * `up`). Unaligned `Value` access through `values[]` is safe on
 * x86-64 -- same precedent as heap.h's LL_ATTR_SIZE=12 packing.
 * Never use `sizeof(Env)` for an allocation size; nothing in this
 * codebase does (see ll_env_alloc/env_arena.S, which both compute
 * the real size from `nslots` directly). */
typedef struct __attribute__((packed)) Env {
    uint32_t up;
    Value values[1]; /* real length is nslots; always arena-allocated, never sizeof()'d */
} Env;

/* M118: base address of the dedicated Env arena -- 0 until the first
 * Env is ever allocated (ll_env_arena_alloc self-initializes lazily
 * on first use; see env_arena.S). Compress a real `Env *` to what
 * belongs in the `up` field above with
 * `(uint32_t)((uintptr_t)e - (uintptr_t)ll_env_arena_base)` (0 stays
 * 0, i.e. NULL, unconditionally -- never subtract from a NULL `e`). */
extern void *ll_env_arena_base;

/* Implemented in src/asm/env.S. Provided as tested, C-callable primitives;
 * the hot dispatch handlers (M3 op_var_ref/op_let/op_call) inline the same
 * walk/bind logic directly rather than paying a `call` per use. */
Env *ll_env_alloc(Env *up, int64_t nslots);
Value ll_env_lookup(Env *env, int64_t depth, int64_t slot);

#endif /* __ASSEMBLER__ */

#endif /* LL_ENV_H */
