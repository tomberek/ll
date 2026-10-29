/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_IRPASS_H
#define LL_IRPASS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "ll/ir.h"

/* Bytecode-to-bytecode optimization-pass framework. Operates on an
 * IrBuilder's accumulated, not-yet-linked ops array; ib_finish() runs
 * every currently registered pass (ll_irpass_register) to a fixpoint over
 * b->ops[0..b->len) immediately before its own ir_link() call.
 *
 * Each individual pass lives in its own small .c file (e.g. irpass_
 * cfold.c, irpass_dce_if.c, ...), each exposing one
 * `ll_irpass_register_<name>(void)` entry point that calls
 * ll_irpass_register() with its own function + a short human-readable
 * name. main.c/irdump.c call whichever of these registration functions
 * they want active; nothing auto-registers itself just by being linked
 * in.
 */

/* Returns 1 if it rewrote/removed anything on this call, 0 if it found
 * nothing left to do. May shrink *len via ll_irpass_remove below; may NOT
 * grow *len (a pass that needs to insert ops should instead rewrite via
 * the IrBuilder API before ib_finish, outside this framework's scope).
 *
 * `pool`/`pool_len`/`pool_cap`: a pass may read an OP_PUSH_CONST's pool
 * entry (`(*pool)[ops[i].b]`) and append a freshly-built constant via
 * ll_irpass_pool_append below, which may realloc `*pool` -- hence
 * `int64_t **pool`, not `int64_t *pool`. */
typedef int (*LlIrPass)(IrOp *ops, size_t *len, int64_t **pool, size_t *pool_len, size_t *pool_cap);

/* Registers `pass` under `name` (a short human-readable label, must stay
 * alive for the process lifetime, so pass a string literal) to run (to a
 * fixpoint, alongside every other registered pass, in registration order)
 * inside every subsequent ib_finish() call. Not thread-safe. */
void ll_irpass_register(const char *name, LlIrPass pass);

/* Drops every registered pass, restoring ib_finish() to its no-passes
 * default. Does NOT reset fire-count/firing-location stats
 * (ll_irpass_reset_stats does that separately). */
void ll_irpass_clear(void);

/* Runs every currently-registered pass over ops[0..*len) to a fixpoint
 * (each pass re-run until a full round makes no further change, capped at
 * LL_IRPASS_MAX_ITERS so a pass that can't reach a fixpoint can't hang
 * compilation). This is what ib_finish() calls internally; exposed
 * directly too so a standalone tool can apply the same pipeline without
 * going through the full builder API. Returns 1 if anything changed
 * across the whole run.
 *
 * `pool`/`pool_len`/`pool_cap`: forwarded verbatim to every registered
 * pass each call -- see LlIrPass's own comment above. */
int ll_irpass_run(IrOp *ops, size_t *len, int64_t **pool, size_t *pool_len, size_t *pool_cap);

/* The one shrink primitive a pass should use to remove ops: deletes
 * `count` ops starting at `start` from ops[0..*len), decrementing *len,
 * and fixes up EVERY jump-delta-bearing op (OP_IF/OP_JMP/OP_WITH_TRY/
 * OP_SELECT_OR/OP_SELECT_DYNAMIC_OR -- see ir.h's own per-op comments;
 * exhaustive, nothing else in OP_COUNT's enum carries a self-relative
 * index delta) in the WHOLE array, including ones before `start` whose
 * target lands after `start+count`, so every jump still lands on the same
 * logical op it did before the removal. Also logs a firing-location entry
 * (see below) and re-shifts every already-logged one, same rule as the
 * jump-target fixup. Fatal (exit 1) if any op's target would land INSIDE
 * the removed range -- a pass must only ever remove a self-contained
 * sequence with no live jump into its interior; this is checked
 * unconditionally, not just under a debug build, since a pass bug here
 * would otherwise silently miscompile. */
void ll_irpass_remove(IrOp *ops, size_t *len, size_t start, size_t count);

#ifdef LL_IRDUMP_LINE_TRACKING
/* irdump's own variant build only (see Makefile): registers the CURRENT
 * program's own per-op source-position array so ll_irpass_remove
 * above shifts it in lockstep with `ops` across every removal, the same
 * way it already keeps the firing log and jump targets aligned. */
void ll_irpass_set_line_array(uint32_t *lines);
#endif

/* The sibling GROW primitive for the pool (as ll_irpass_remove is for
 * ops): appends `v` to *pool[0..*pool_len), growing *pool_cap via realloc
 * when needed, and returns the new entry's index (always the previous
 * *pool_len) for the caller to stash into some op's `b` field (e.g.
 * OP_PUSH_CONST). Safe to call even when *pool_cap == 0. */
size_t ll_irpass_pool_append(int64_t **pool, size_t *pool_len, size_t *pool_cap, int64_t v);

#define LL_IRPASS_MAX_ITERS 64

/* --- Per-pass firing metrics, for irdump/benchmark reporting --- */

/* Number of currently-registered passes (registration order == index
 * order used by the two accessors below). */
size_t ll_irpass_num_passes(void);
const char *ll_irpass_name(size_t index);
/* How many times this pass's function has returned 1 (i.e. performed one
 * rewrite) since the last ll_irpass_reset_stats() call -- accumulates
 * across every ll_irpass_run() call in between, so a whole top-level
 * compile's TOTAL firing count across the root program and every nested
 * lambda/thunk body (each its own ib_finish() call) is visible as one
 * number, not reset per-program. */
uint64_t ll_irpass_fire_count(size_t index);
/* Zeroes every pass's fire count AND the firing-location log below (see
 * ll_irpass_clear for the separate "which passes are registered"
 * reset). */
void ll_irpass_reset_stats(void);
/* Convenience: prints one line per registered pass with a nonzero fire
 * count ("name: N firings"), or "(no passes fired)" if every registered
 * pass's count is 0. Prints nothing at all if zero passes are
 * registered. */
void ll_irpass_print_stats(FILE *out);

/* --- Firing-location log: WHERE each pass fired, not just how many times.
 *
 * Populated by ll_irpass_remove at the index the kept/folded op ends up
 * at (start-1 by convention -- see irpass_cfold.c). Kept valid against
 * whatever the CURRENT ops array looks like at any point -- every later
 * ll_irpass_remove call also shifts already-logged indices.
 *
 * Disambiguated across separate IrPrograms (root vs. every nested lambda/
 * thunk body, each its own ib_finish() call) via ll_irpass_begin_
 * program/ll_irpass_mark_program below, bracketing ib_finish()'s own
 * ll_irpass_run call -- without that, two different programs' op
 * arrays routinely have OVERLAPPING index ranges (each starts its own
 * indexing at 0), so a later program's own ll_irpass_remove shift-
 * fixup would otherwise silently corrupt an earlier, already-finished
 * program's already-correct locations, not just display them
 * imprecisely. Reset by ll_irpass_reset_stats(), same as fire counts. */
size_t ll_irpass_num_firings(void);
const char *ll_irpass_firing_pass_name(size_t i);
size_t ll_irpass_firing_pass_index(size_t i);
size_t ll_irpass_firing_op_index(size_t i);

/* Call once per ib_finish(), immediately BEFORE its own ll_irpass_run
 * call, to mark where THIS program's own slice of the firing log begins
 * -- bounds ll_irpass_remove's shift-fixup to [that point, now) so it
 * never touches an earlier, already-finished program's entries. */
void ll_irpass_begin_program(void);
/* Call once per ib_finish(), immediately AFTER `prog` is allocated (right
 * after the ll_irpass_run call ll_irpass_begin_program bracketed),
 * to tag every firing-log entry logged since that begin call as
 * belonging to `prog`. A no-op when zero passes are registered or when
 * this program logged nothing. */
void ll_irpass_mark_program(const void *prog);
/* True iff firing-log entry `i` was tagged (via ll_irpass_mark_program)
 * as belonging to `prog`. False if ll_irpass_mark_program was never
 * called for `prog` (e.g. zero passes were registered when it compiled,
 * or it simply logged nothing). Lets a caller with its own `prog` pointer
 * in hand (e.g. irdump, dumping one specific IrProgram) filter
 * ll_irpass_firing_op_index(i) to just the entries that are actually
 * its own, instead of the whole global log. */
int ll_irpass_firing_belongs_to_program(size_t i, const void *prog);

#endif /* LL_IRPASS_H */
