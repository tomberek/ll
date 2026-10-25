/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef ASNIX_IRPASS_H
#define ASNIX_IRPASS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "asnix/ir.h"

/*
 * Bytecode-to-bytecode optimization-pass framework.
 *
 * Operates on an IrBuilder's accumulated, NOT-YET-LINKED ops array --
 * `handler` still holds a small op-id at this point, exactly what
 * ib_emit() just wrote (irbuild.c). ib_finish() runs every currently
 * registered pass (asnix_irpass_register) to a fixpoint over
 * b->ops[0..b->len) immediately before its own ir_link() call -- so EVERY
 * IrProgram this project ever builds (the top-level file, every lambda
 * body, every thunk body: each is its own IrBuilder, ib_finish()-ed
 * independently) gets the same treatment for free, with no change needed
 * at any of compiler.c's many ib_new()/ib_finish() call sites.
 *
 * With ZERO passes registered (the default -- asnix's own normal
 * production run never registers any), ib_finish()'s behavior is
 * byte-for-byte identical to before this framework existed. Registering a
 * pass is an explicit, opt-in action a test/benchmark harness (or
 * ASNIX_IR_OPT=1, main.c) takes before compiling, not a change to the
 * shipped interpreter's default behavior.
 *
 * Each individual pass lives in its OWN small .c file (e.g. irpass_
 * cfold.c, irpass_dce_if.c, ...), each exposing one
 * `asnix_irpass_register_<name>(void)` entry point that calls
 * asnix_irpass_register() with its own function + a short human-readable
 * name -- deliberately one pass per file so several agents/contributors
 * can each add a new pass without touching a shared file (this session's
 * own standing lesson: concurrent edits to ONE shared file is exactly the
 * collision class to avoid -- see docs/architecture.md's M110-era
 * incidents). main.c/irdump.c call whichever of these registration
 * functions they want active; nothing auto-registers itself just by
 * being linked in.
 */

/* Returns 1 if it rewrote/removed anything on THIS call (one rewrite per
 * call is the expected/simplest style -- find one occurrence, fix it,
 * return 1, let the fixpoint driver call again from a clean state), 0 if
 * it found nothing left to do. May shrink `*len` via asnix_irpass_remove
 * below; may NOT grow `*len` (no growable-OPS-array support here -- every
 * pass this framework ships only removes or rewrites ops in place; a pass
 * that needs to INSERT new ops should instead rewrite via the IrBuilder
 * API before ib_finish, outside this framework's scope).
 *
 * `pool`/`pool_len`/`pool_cap` (strconcat-fold extension): the SAME three
 * fields `ib_finish` is about to hand off to the IrBuilder's own
 * `IrProgram`, passed through by pointer so a pass can both READ an
 * OP_PUSH_CONST's pool entry (`(*pool)[ops[i].b]`, a Value.raw word --
 * needed to check the pool entry's actual runtime Value tag, e.g. "is
 * this genuine String", NOT just something derivable from the op alone)
 * and APPEND a freshly-built constant via asnix_irpass_pool_append below
 * (which may realloc `*pool`, growing `*pool_cap` -- that's why this is
 * `int64_t **pool`, not `int64_t *pool`, unlike the ops array that's
 * never grown by this framework). Every pass, including ones that never
 * touch the pool (e.g. cfold's own int-only fold), still takes these
 * three in its signature -- a cross-cutting change to AsnixIrPass itself,
 * not a per-pass opt-in, so the one `asnix_irpass_run` call site
 * (ib_finish, irbuild.c) only ever has ONE pool to thread through to
 * every registered pass uniformly. */
typedef int (*AsnixIrPass)(IrOp *ops, size_t *len, int64_t **pool, size_t *pool_len, size_t *pool_cap);

/* Registers `pass` under `name` (a short human-readable label, e.g.
 * "cfold" -- used by asnix_irpass_print_stats/irdump's annotated dump,
 * must stay alive for the process lifetime, so pass a string literal) to
 * run (to a fixpoint, alongside every other registered pass, in
 * registration order) inside every subsequent ib_finish() call. Not
 * thread-safe -- same convention as every other process-global table in
 * this codebase (symtab, the import cache, ...); asnix is
 * single-threaded throughout. */
void asnix_irpass_register(const char *name, AsnixIrPass pass);

/* Testing/benchmarking convenience: drops every registered pass,
 * restoring ib_finish() to its no-passes default. Lets a harness compile
 * the SAME source once with the pipeline on and once with it off, in the
 * same process, for a paired before/after comparison. Does NOT reset
 * fire-count/firing-location stats (asnix_irpass_reset_stats does that
 * separately) -- clearing registration and resetting the firing data are
 * independent operations. */
void asnix_irpass_clear(void);

/* Runs every currently-registered pass over ops[0..*len) to a fixpoint
 * (each pass re-run until a full round makes no further change, capped at
 * ASNIX_IRPASS_MAX_ITERS so a pass that can't reach a fixpoint can't hang
 * compilation). This is what ib_finish() calls internally; exposed
 * directly too so a standalone tool can apply the same pipeline without
 * going through the full builder API. Returns 1 if anything changed
 * across the whole run. Every successful (returns 1) call to a given
 * pass increments that pass's own fire-count (see below) by one, and
 * (via asnix_irpass_remove) may append to the firing-location log too.
 *
 * `pool`/`pool_len`/`pool_cap`: forwarded verbatim to every registered
 * pass each call -- see AsnixIrPass's own comment above. */
int asnix_irpass_run(IrOp *ops, size_t *len, int64_t **pool, size_t *pool_len, size_t *pool_cap);

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
void asnix_irpass_remove(IrOp *ops, size_t *len, size_t start, size_t count);

#ifdef ASNIX_IRDUMP_LINE_TRACKING
/* irdump's own variant build only (see Makefile): registers the CURRENT
 * program's own per-op source-position array so asnix_irpass_remove
 * above shifts it in lockstep with `ops` across every removal, the same
 * way it already keeps the firing log and jump targets aligned. A real
 * production build of irpass.c never defines this macro, so neither this
 * declaration nor the shift logic inside asnix_irpass_remove exists
 * there at all -- measured directly that even a single always-false
 * runtime check here was not free enough to leave in unconditionally. */
void asnix_irpass_set_line_array(uint32_t *lines);
#endif

/* The sibling GROW primitive for the pool (as asnix_irpass_remove is for
 * ops): appends `v` to *pool[0..*pool_len), doubling *pool_cap (starting
 * from 8, same growth policy as irbuild.c's own ib_pool_add -- these ARE
 * the live IrBuilder's own pool fields, mid-construction) via realloc
 * when needed, and returns the new entry's index (always the previous
 * *pool_len) for the caller to stash into some op's `b` field (e.g.
 * OP_PUSH_CONST). Safe to call even when *pool_cap == 0. */
size_t asnix_irpass_pool_append(int64_t **pool, size_t *pool_len, size_t *pool_cap, int64_t v);

#define ASNIX_IRPASS_MAX_ITERS 64

/* --- Per-pass firing metrics, for irdump/benchmark reporting --- */

/* Number of currently-registered passes (registration order == index
 * order used by the two accessors below). */
size_t asnix_irpass_num_passes(void);
const char *asnix_irpass_name(size_t index);
/* How many times this pass's function has returned 1 (i.e. performed one
 * rewrite) since the last asnix_irpass_reset_stats() call -- accumulates
 * across every asnix_irpass_run() call in between, so a whole top-level
 * compile's TOTAL firing count across the root program and every nested
 * lambda/thunk body (each its own ib_finish() call) is visible as one
 * number, not reset per-program. */
uint64_t asnix_irpass_fire_count(size_t index);
/* Zeroes every pass's fire count AND the firing-location log below (see
 * asnix_irpass_clear for the separate "which passes are registered"
 * reset). Call once before a compile whose firing data you want isolated
 * from any earlier one in the same process. */
void asnix_irpass_reset_stats(void);
/* Convenience: prints one line per registered pass with a nonzero fire
 * count ("name: N firings"), or "(no passes fired)" if every registered
 * pass's count is 0. Prints nothing at all if zero passes are
 * registered. */
void asnix_irpass_print_stats(FILE *out);

/* --- Firing-location log: WHERE each pass fired, not just how many times.
 *
 * Populated by asnix_irpass_remove at the index the kept/folded op ends up
 * at (start-1 by convention -- every pass so far rewrites ops[i] in place
 * then removes ops[i+1..i+1+count), so the fold's result lives at i ==
 * start-1; see irpass_cfold.c). Kept valid against whatever the CURRENT
 * ops array looks like at any point -- every later asnix_irpass_remove
 * call also shifts already-logged indices, so by the time a compile
 * finishes, every logged index is still correct for the FINAL array
 * irdump actually prints.
 *
 * Disambiguated across separate IrPrograms (root vs. every nested lambda/
 * thunk body, each its own ib_finish() call) via asnix_irpass_begin_
 * program/asnix_irpass_mark_program below, bracketing ib_finish()'s own
 * asnix_irpass_run call -- without that, two different programs' op
 * arrays routinely have OVERLAPPING index ranges (each starts its own
 * indexing at 0), so a later program's own asnix_irpass_remove shift-
 * fixup would otherwise silently corrupt an earlier, already-finished
 * program's already-correct locations, not just display them
 * imprecisely -- found concretely on a real file where this produced
 * firing markers on lines in programs that never fired at all. Reset by
 * asnix_irpass_reset_stats(), same as fire counts. */
size_t asnix_irpass_num_firings(void);
const char *asnix_irpass_firing_pass_name(size_t i);
size_t asnix_irpass_firing_pass_index(size_t i);
size_t asnix_irpass_firing_op_index(size_t i);

/* Call once per ib_finish(), immediately BEFORE its own asnix_irpass_run
 * call, to mark where THIS program's own slice of the firing log begins
 * -- bounds asnix_irpass_remove's shift-fixup to [that point, now) so it
 * never touches an earlier, already-finished program's entries. */
void asnix_irpass_begin_program(void);
/* Call once per ib_finish(), immediately AFTER `prog` is allocated (right
 * after the asnix_irpass_run call asnix_irpass_begin_program bracketed),
 * to tag every firing-log entry logged since that begin call as
 * belonging to `prog`. A no-op when zero passes are registered (same
 * "byte-for-byte identical by default" guarantee as the rest of this
 * framework) or when this program logged nothing. */
void asnix_irpass_mark_program(const void *prog);
/* True iff firing-log entry `i` was tagged (via asnix_irpass_mark_program)
 * as belonging to `prog`. False if asnix_irpass_mark_program was never
 * called for `prog` (e.g. zero passes were registered when it compiled,
 * or it simply logged nothing). Lets a caller with its own `prog` pointer
 * in hand (e.g. irdump, dumping one specific IrProgram) filter
 * asnix_irpass_firing_op_index(i) to just the entries that are actually
 * its own, instead of the whole global log. */
int asnix_irpass_firing_belongs_to_program(size_t i, const void *prog);

#endif /* ASNIX_IRPASS_H */
