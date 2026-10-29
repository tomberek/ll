/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
// Shared state and abort routines backing include/ll/stackcheck.h's
// LL_CHECK_VALSTACK/LL_CHECK_RETSP macros, used at every VALSTACK/
// RETSP push site. Both dispatch.S's top-level eval loop and force.S's
// nested-forcing context write ll_valstack_limit/ll_retsp_limit to
// "one past the last valid byte" of whichever buffer they just
// allocated (force.S saves and restores the previous values around
// its own nested use, like it already does for %r12/%r14) so the
// same shared op handler code enforces the right cap in either
// context. Exceeding either cap aborts cleanly via ll_fatal instead
// of silently corrupting whatever the arena allocates next.
    .bss
    .align 8
    .globl ll_valstack_limit
    .type ll_valstack_limit, @object
ll_valstack_limit:
    .quad 0
    .size ll_valstack_limit, 8

    .align 8
    .globl ll_retsp_limit
    .type ll_retsp_limit, @object
ll_retsp_limit:
    .quad 0
    .size ll_retsp_limit, 8

    // Head of an intrusive "currently IN_PROGRESS" Thunk* stack,
    // linked through each such thunk's OWN `env` field (see ir.h's
    // LL_THUNK_OFF_ENV comment -- safe to repurpose once IN_PROGRESS,
    // since the original env was already consumed to jump into the
    // thunk's body). Every site that transitions a thunk UNFORCED ->
    // IN_PROGRESS (op_var_ref.S, op_with.S, op_select.S, op_select_or.S,
    // force.S) pushes itself here; the ONE site that transitions
    // IN_PROGRESS -> FORCED (op_call.S's ll_op_ret, which already
    // memoizes every force uniformly regardless of which op started it)
    // pops itself back off. `builtins.tryEval` (tryeval.c) walks this
    // list from its own saved marker up to the live head when catching a
    // `throw`/failed `assert` that unwound via longjmp -- the thunks in
    // that range are exactly the ones left stuck IN_PROGRESS by frames
    // the longjmp skipped over, which get marked FAILED (LL_THUNK_
    // STATE_FAILED) instead of being mistaken for a genuine blackhole on
    // the next force.
    .align 8
    .globl ll_inprogress_head
    .type ll_inprogress_head, @object
ll_inprogress_head:
    .quad 0
    .size ll_inprogress_head, 8

    .section .rodata
.Lmsg_valstack_overflow:
    .ascii "ll: value stack overflow (too many live pushed values), aborting\n"
    .equ LL_MSG_VALSTACK_OVERFLOW_LEN, . - .Lmsg_valstack_overflow
.Lmsg_retsp_overflow:
    .ascii "ll: call/force stack overflow (too much nested recursion), aborting\n"
    .equ LL_MSG_RETSP_OVERFLOW_LEN, . - .Lmsg_retsp_overflow

    .text

    .globl ll_valstack_overflow
    .type ll_valstack_overflow, @function
ll_valstack_overflow:                // never returns
    leaq .Lmsg_valstack_overflow(%rip), %rdi
    movq $LL_MSG_VALSTACK_OVERFLOW_LEN, %rsi
    jmp ll_fatal
    .size ll_valstack_overflow, . - ll_valstack_overflow

    .globl ll_retsp_overflow
    .type ll_retsp_overflow, @function
ll_retsp_overflow:                   // never returns
    leaq .Lmsg_retsp_overflow(%rip), %rdi
    movq $LL_MSG_RETSP_OVERFLOW_LEN, %rsi
    jmp ll_fatal
    .size ll_retsp_overflow, . - ll_retsp_overflow
