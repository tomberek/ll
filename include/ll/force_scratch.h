/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_FORCE_SCRATCH_H
#define LL_FORCE_SCRATCH_H

/*
 * Sizes of the two per-call scratch buffers src/asm/force.S's
 * ll_force_for_print carves out to run its own self-contained mini
 * dispatch context -- shared with force_scratch.c (below) so the two
 * never drift apart, same dual-use-header convention as arena.h's
 * LL_ARENA_OFF_* constants.
 */
#define LL_FORCE_VALSTACK_BYTES 262144 /* 32,768 Values, 256 KiB */
#define LL_FORCE_RETSTACK_BYTES 65536  /* 2,048 call frames, 64 KiB */

#ifndef __ASSEMBLER__

#include <stdint.h>

/*
 * ll_force_for_print's nested-recursion depth (src/asm/force.S's
 * ll_force_depth: 0 for a top-level, non-nested call; N for a call
 * reached from inside N other still-active force_for_print frames)
 * doubles as an index into a per-depth cache of scratch buffers here.
 * Caching one buffer pair PER DEPTH (grown lazily) is safe because
 * depth is strictly LIFO: a slot is only ever "in use" while some live
 * frame at that exact depth hasn't returned yet, and the asm side
 * already enforces that discipline by construction (push on entry, pop
 * on exit).
 */
void *ll_force_scratch_valstack(int64_t depth);
void *ll_force_scratch_retstack(int64_t depth);

/*
 * src/asm/apply.S's ll_apply (the re-entrant "call this Value from C"
 * entry point `map`/`filter`/`foldl'`/etc. use) uses a separate depth
 * counter/cache from the force_for_print one above -- apply.S's own
 * header comment is explicit that this context is never shared with an
 * enclosing force_for_print (or eval, or another apply) invocation.
 * Same buffer sizes (LL_FORCE_VALSTACK_BYTES/LL_FORCE_RETSTACK_BYTES
 * above happen to equal apply.S's own LL_APPLY_VALSTACK_BYTES/
 * LL_APPLY_RETSTACK_BYTES equ constants -- reused here rather than
 * duplicating the same two numbers under a second name).
 */
void *ll_apply_scratch_valstack(int64_t depth);
void *ll_apply_scratch_retstack(int64_t depth);

#endif /* __ASSEMBLER__ */

/* Must match dispatch.S's own equ constants of the same size. */
#define LL_EVAL_VALSTACK_BYTES 1048576 /* 131,072 Values, 1 MiB -- must match dispatch.S's own equ */
#define LL_EVAL_RETSTACK_BYTES 1048576 /* 32,768 call frames, 1 MiB -- must match dispatch.S's own equ */

#ifndef __ASSEMBLER__
void *ll_eval_scratch_valstack(int64_t depth);
void *ll_eval_scratch_retstack(int64_t depth);
#endif /* __ASSEMBLER__ */

#endif /* LL_FORCE_SCRATCH_H */
