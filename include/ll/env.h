/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_ENV_H
#define LL_ENV_H

/* Env{up; values[]}: a flat per-scope frame, chained via `up` for lexical
 * lookup (De Bruijn depth,slot resolved at compile time). One frame per
 * let/lambda-call.
 *
 * `up` is a 32-bit offset into the dedicated, bounded `ll_env_arena`
 * (env_arena.S), not a raw 64-bit pointer -- Env is never itself a tagged
 * Value, so it doesn't need to honor Value.raw's 8-byte/3-tag-bit
 * alignment invariant the general arena's `round_up_8` guarantees for
 * everything else. 0 always means NULL -- the dedicated arena's bump
 * cursor deliberately starts at base+8, never base+0, so a real
 * allocation can never collide with the NULL sentinel. */
#define LL_ENV_OFF_UP     0
#define LL_ENV_OFF_VALUES 4

/* `with`: a with-frame is an ordinary 1-slot Env{up;values[0]=attrs} with
 * one difference -- its OWN `up` field has this tag bit OR'd in. Every
 * walk that blindly chases `up` across a *generic* hop (not specifically
 * looking for a with-frame -- e.g. OP_VAR_REF's depth walk, which may
 * pass straight through a with-frame on its way to an outer lexical
 * binding) must mask this bit back off AFTER decompressing, since
 * nothing else ever sets it. */
#define LL_ENV_WITH_TAG   1

/* Dedicated, bounded Env arena size -- see env_arena.S. A hard abort
 * (ll_fatal) if a program's live Env count ever actually reaches it.
 * Must stay well under 4 GiB (2^32) so every offset this arena ever
 * hands out always fits in the 32-bit field above with room to spare. */
#define LL_ENV_ARENA_SIZE 1073741824

#ifndef __ASSEMBLER__

#include <stdint.h>
#include "ll/value.h"

/* `up` is a compressed 32-bit offset (see above), not `struct Env *up` --
 * packed so `values[]` sits at byte offset 4, matching LL_ENV_OFF_VALUES
 * exactly (an un-packed struct would insert 4 bytes of alignment padding
 * before `values[0]` to keep `Value` 8-aligned, silently defeating the
 * whole point of shrinking `up`). Unaligned `Value` access through
 * `values[]` is safe on x86-64 -- same precedent as heap.h's
 * LL_ATTR_SIZE=12 packing. Never use `sizeof(Env)` for an allocation
 * size; nothing in this codebase does (see ll_env_alloc/env_arena.S,
 * which both compute the real size from `nslots` directly). */
typedef struct __attribute__((packed)) Env {
    uint32_t up;
    Value values[1]; /* real length is nslots; always arena-allocated, never sizeof()'d */
} Env;

/* Base address of the dedicated Env arena -- 0 until the first Env is
 * ever allocated. Compress a real `Env *` to what belongs in the `up`
 * field above with
 * `(uint32_t)((uintptr_t)e - (uintptr_t)ll_env_arena_base)` (0 stays
 * 0, i.e. NULL, unconditionally -- never subtract from a NULL `e`). */
extern void *ll_env_arena_base;

Env *ll_env_alloc(Env *up, int64_t nslots);
Value ll_env_lookup(Env *env, int64_t depth, int64_t slot);

#endif /* __ASSEMBLER__ */

#endif /* LL_ENV_H */
