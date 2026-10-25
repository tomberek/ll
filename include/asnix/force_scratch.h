/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef ASNIX_FORCE_SCRATCH_H
#define ASNIX_FORCE_SCRATCH_H

/*
 * Sizes of the two per-call scratch buffers src/asm/force.S's
 * asnix_force_for_print carves out to run its own self-contained mini
 * dispatch context -- shared with force_scratch.c (below) so the two
 * never drift apart, same dual-use-header convention as arena.h's
 * ASNIX_ARENA_OFF_* constants.
 */
/*
 * M80: left at the ORIGINAL sizes, unlike dispatch.S's own (see that
 * file's ASNIX_VALSTACK_BYTES/ASNIX_RETSTACK_BYTES comment) -- a real
 * high-water-mark measurement against five real nixpkgs packages found
 * a tiny peak here too (1,680 bytes valstack / 960 bytes retstack), but
 * two independent findings while verifying a shrink proved that sample
 * isn't representative enough to size against, matching apply.S's own
 * identical conclusion for its ASNIX_APPLY_VALSTACK_BYTES/
 * ASNIX_APPLY_RETSTACK_BYTES (see that file's comment for the full
 * reasoning -- this buffer is intentionally the same size, reused here
 * rather than duplicating the same two numbers under a second name):
 * `tests/e2e/m9_large_list.nix` needs ~120,000 bytes of valstack
 * (a 15,000-element literal list is constructed *through this exact
 * buffer*, since op_call.S forces every primop argument via
 * asnix_force_for_print), and real Nix's own `max-call-depth` default
 * of 10,000 function calls is already comfortably above this buffer's
 * original 2,048-call-frame retstack capacity -- shrinking either
 * further without real-usage evidence it's safe would only widen an
 * already-known gap.
 */
#define ASNIX_FORCE_VALSTACK_BYTES 262144 /* 32,768 Values, 256 KiB */
#define ASNIX_FORCE_RETSTACK_BYTES 65536  /* 2,048 call frames, 64 KiB */

#ifndef __ASSEMBLER__

#include <stdint.h>

/*
 * asnix_force_for_print's nested-recursion depth (src/asm/force.S's
 * asnix_force_depth: 0 for a top-level, non-nested call; N for a call
 * reached from inside N other still-active force_for_print frames)
 * doubles as an index into a per-depth cache of scratch buffers here --
 * found chasing d.nix's own real-workload slowness: every call used to
 * mint a BRAND NEW pair of buffers from the never-free bump arena
 * (confirmed via `perf stat`: ~169k page faults / ~20 GB of one-shot
 * arena growth evaluating d.nix, ~33% of all page faults attributed
 * directly to asnix_force_for_print itself). A single depth==0-only
 * cache (the first attempt at fixing this) barely helped, because
 * profiling showed most real calls here ARE nested -- derivation.c's
 * argument processing calls asnix_force_for_print on a thunk whose own
 * body ends up back in derivation.c calling asnix_force_for_print again
 * on a nested value, routinely several levels deep. Caching one buffer
 * pair PER DEPTH instead (grown lazily, like eval_file.c's import
 * cache) is safe because depth is strictly LIFO: a slot is only ever
 * "in use" while some live frame at that exact depth hasn't returned
 * yet, and the asm side already enforces that discipline by construction
 * (push on entry, pop on exit) -- same precedent as every other
 * linear/array-indexed cache in this codebase, just indexed by depth
 * instead of scanned by key, since depth is already a dense small
 * integer with no need for a search.
 */
void *asnix_force_scratch_valstack(int64_t depth);
void *asnix_force_scratch_retstack(int64_t depth);

/*
 * src/asm/apply.S's asnix_apply (the re-entrant "call this Value from C"
 * entry point `map`/`filter`/`foldl'`/etc. use) was modeled on
 * asnix_force_for_print's own "own fresh VALSTACK+RETSP via
 * asnix_arena_alloc" pattern -- including the exact same per-call-fresh-
 * allocation inefficiency this file's own functions above were added to
 * fix, just never updated to match once that fix landed. Found chasing
 * a memory-usage question (asnix's own `asnix_arena.total_allocated`
 * read 4.4 GB evaluating d.nix against only ~227 MB of actual resident
 * memory -- the gap is virtual-address-space waste from unused mmap'd
 * pages, harmless on its own, but `asnix_apply` turned out to be BY FAR
 * the larger of the two culprits: it's reached far more often than
 * force_for_print, via every `map`/`filter`/`foldl'`-style builtin call
 * in a real nixpkgs evaluation). Separate depth counter/cache from the
 * force_for_print one above -- apply.S's own header comment is explicit
 * that this context is never shared with an enclosing force_for_print
 * (or eval, or another apply) invocation, so keeping the two caches
 * independent matches that existing design intent rather than
 * introducing new coupling between them. Same buffer sizes
 * (ASNIX_FORCE_VALSTACK_BYTES/ASNIX_FORCE_RETSTACK_BYTES above happen to
 * equal apply.S's own ASNIX_APPLY_VALSTACK_BYTES/ASNIX_APPLY_RETSTACK_BYTES
 * equ constants -- reused here rather than duplicating the same two
 * numbers under a second name).
 */
void *asnix_apply_scratch_valstack(int64_t depth);
void *asnix_apply_scratch_retstack(int64_t depth);

#endif /* __ASSEMBLER__ */

/*
 * src/asm/dispatch.S's own asnix_eval -- the main entry point, called
 * fresh for EVERY file import (eval_file.c's asnix_eval_file et al.),
 * which can nest (`import` inside an imported file) -- had the exact
 * same per-call-fresh-allocation pattern as force_for_print/apply
 * above, at 10x the size (8 MiB + 2 MiB vs. 256 KiB + 64 KiB each): a
 * full nixpkgs evaluation imports hundreds of distinct files, so this
 * turned out to be the single largest contributor to asnix_arena's
 * total_allocated figure, well above apply.S's own (already-fixed)
 * share.
 */
/* M80: right-sized, see dispatch.S's own equ (which this must match) for
 * the measurement/margin reasoning (including the real-Nix
 * `max-call-depth` cross-check that sized RETSTACK, not the raw
 * per-package measurement alone). */
#define ASNIX_EVAL_VALSTACK_BYTES 1048576 /* 131,072 Values, 1 MiB -- must match dispatch.S's own equ */
#define ASNIX_EVAL_RETSTACK_BYTES 1048576 /* 32,768 call frames, 1 MiB -- must match dispatch.S's own equ */

#ifndef __ASSEMBLER__
void *asnix_eval_scratch_valstack(int64_t depth);
void *asnix_eval_scratch_retstack(int64_t depth);
#endif /* __ASSEMBLER__ */

#endif /* ASNIX_FORCE_SCRATCH_H */
