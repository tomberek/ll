/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_TRYEVAL_H
#define LL_TRYEVAL_H

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

/*
 * M30: a narrow, targeted catchable-throw mechanism for `builtins.tryEval`
 * -- NOT a general exception system. Real Nix's own `tryEval` only ever
 * catches `AssertionError` and its one subclass `ThrownError` (confirmed
 * directly in cppnix's source, `~/nix/src/libexpr/primops.cc`'s
 * `prim_tryEval`: `catch (AssertionError & e)`, and
 * `~/nix/src/libexpr/include/nix/expr/eval-error.hh`'s
 * `MakeError(ThrownError, AssertionError)`) -- i.e. exactly `throw` and a
 * failed `assert`, and nothing else: NOT type errors, NOT infinite
 * recursion, NOT missing attributes, and explicitly NOT `abort` (verified
 * directly against `nix-instantiate --eval`: `tryEval (abort "x")` still
 * aborts the whole process unconditionally). Every other fatal path in
 * this codebase (`ll_fatal`, used for all of those) is completely
 * unaffected by this file -- only `prim_throw` (builtins.c) and
 * `OP_ASSERT_FAIL`'s handler (op_assert.S) route through here instead.
 *
 * Implemented with plain `setjmp`/`longjmp`, not a sweeping redesign:
 * this project's own dispatch loop already keeps its live evaluator
 * state (VALSTACK/POOL/RETSP: %r12/%r13/%r14) in registers deliberately
 * chosen to be SysV callee-saved specifically so they survive an ordinary
 * C call for free (see docs/abi.md) -- which also means a plain
 * `longjmp` correctly rewinds them back to their `setjmp`-time values,
 * discarding whatever partial evaluation happened since, with no extra
 * bookkeeping needed. `%rsi`/`%rdi` (IP/ENV) need no special handling
 * either: `prim_tryEval` is an ordinary primop called through
 * `op_call.S`'s existing save-IP/ENV-around-the-call convention, so once
 * `prim_tryEval` returns normally (whichever way it got there), the
 * caller's own push/pop restores them regardless of what happened inside.
 *
 * M82 (see docs/architecture.md): previously-documented limitation, now
 * fixed -- if the thrown expression's evaluation forced some OTHER thunk
 * partway through (not just the thunk `tryEval` was directly asked to
 * evaluate), that thunk used to stay marked IN_PROGRESS forever (the
 * normal "finish forcing, mark FORCED" step never runs for it, since the
 * `longjmp` back to here skips every intervening op_*.S/force.S
 * epilogue) -- referencing that *same* thunk object again later would
 * incorrectly blackhole (found via numtide/nix-conformance's
 * eval-okay-tryeval-failed-thunk-reeval.nix: "Since Nix 2.34, errors are
 * memoized" -- real Nix re-raises the SAME cached failure on a second
 * force, rather than either re-running the thunk's body or treating it
 * as a genuine cycle). Fixed via `ll_inprogress_head`
 * (stacklimits.S): every op_*.S/force.S site that marks a thunk
 * IN_PROGRESS also pushes it onto this intrusive stack (linked through
 * the thunk's own, now-unused `env` field -- see ir.h's LL_THUNK_OFF_
 * ENV comment), and `ll_tryeval_save_inprogress_head`/
 * `ll_tryeval_unwind_inprogress` below let `prim_tryEval`
 * (builtins_tryeval.c) walk exactly the range of thunks that became
 * IN_PROGRESS during ITS OWN dynamic extent (not any OUTER, still-
 * legitimately-in-progress force further up the call chain) and mark
 * them FAILED instead -- re-forcing a FAILED thunk re-raises
 * (catchable again) rather than re-running the body or blackholing; see
 * each op_*.S/force.S's own `.L*_reraise` label.
 */

#define LL_TRYEVAL_MAX_DEPTH 256

/* Pushes `buf` as the target for the next ll_tryeval_raise_or_fatal
 * call (supports nested `tryEval`s: a LIFO stack of buffer pointers).
 * `buf` must be `setjmp`'d by the caller itself, immediately after this
 * call returns -- `setjmp` can't be meaningfully wrapped in a helper
 * function, since the saved context would belong to the wrong stack
 * frame. Exits cleanly (not a tryEval-catchable condition itself) if
 * nested too deeply -- matching this project's existing fixed-size-
 * stack-array convention elsewhere (e.g. AST_VAR's with-chain limit). */
void ll_tryeval_push(jmp_buf *buf);

/* Pops the stack pushed by ll_tryeval_push -- call once tryEval's own
 * try/catch region is done, on EITHER path (the forced value came back
 * normally, or the catch branch ran after a longjmp here). */
void ll_tryeval_pop(void);

/* `throw`/a failed `assert` reaches here. If a `tryEval` is currently
 * active (the stack above is non-empty), longjmps to its buffer --
 * never returns. Otherwise falls back to exactly ll_fatal's own
 * behavior (write `msg` to stderr, exit 1) -- never returns either way. */
_Noreturn void ll_tryeval_raise_or_fatal(const char *msg, size_t len);

/* M82: snapshot of `ll_inprogress_head` (stacklimits.S) -- call BEFORE
 * forcing the expression `tryEval` was asked to evaluate (alongside the
 * existing VALSTACK/RETSP-limit saves, builtins_tryeval.c), so a later
 * `ll_tryeval_unwind_inprogress` call knows exactly which thunks
 * became IN_PROGRESS strictly within this `tryEval`'s own dynamic
 * extent. */
int64_t ll_tryeval_save_inprogress_head(void);

/* M82: walks `ll_inprogress_head`'s intrusive stack from its CURRENT
 * value down to `saved_head` (as returned by
 * ll_tryeval_save_inprogress_head before the force that just got
 * caught), marking every thunk in that range LL_THUNK_STATE_FAILED
 * (instead of leaving it stuck IN_PROGRESS) and restoring
 * `ll_inprogress_head` to `saved_head`. Call ONLY from the caught
 * (longjmp'd-to) branch -- on the normal-return path every thunk already
 * popped itself via its own successful OP_RET, so the head is already
 * back to `saved_head` with nothing to unwind. */
void ll_tryeval_unwind_inprogress(int64_t saved_head);

#endif /* LL_TRYEVAL_H */
