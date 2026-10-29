/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_STACKCHECK_H
#define LL_STACKCHECK_H

/*
 * Bounds checks for the two explicit stacks the dispatch loop keeps off
 * the native machine stack: VALSTACK (%r12, the operand stack every
 * OP_PUSH_{INT,TRUE,FALSE,NULL,CONST}, OP_VAR_REF, OP_SELECT, OP_CALL's
 * result, OP_LAMBDA, and OP_MK_THUNK all push a Value onto) and RETSP
 * (%r14, the explicit call-frame stack
 * OP_CALL/OP_VAR_REF's and OP_SELECT's thunk-force path push a RetFrame
 * onto). Both buffers are single fixed-size bump-arena allocations (see
 * src/asm/dispatch.S and src/asm/force.S) with no dynamic growth -- before
 * this header existed, NOTHING checked whether a push still fit, so
 * exceeding either buffer silently corrupted whatever the shared arena
 * happened to allocate next instead of failing loudly (see
 * docs/architecture.md's "Known bugs" list).
 *
 * `ll_valstack_limit`/`ll_retsp_limit` (defined in
 * src/asm/stacklimits.S) hold "one past the last valid byte" of whichever
 * buffer is *currently* active. There are two separate dispatch contexts
 * that can be active at a time -- the top-level ll_eval loop
 * (dispatch.S, large buffers) and force.S's smaller, separate nested
 * -forcing context -- and both write these same two globals when they set
 * up %r12/%r14, saving/restoring the previous values around the nested
 * case exactly like %r12/%r14 themselves, so this one pair of checks is
 * correct no matter which context is running the shared op handlers.
 *
 * `scratch` must name a register that is dead at the call site (i.e. not
 * holding a live value the handler still needs) -- typically %rcx, %rdx,
 * or %r8-%r11 at a typical push site; see the call sites for the exact
 * liveness reasoning at each one.
 */

#ifdef __ASSEMBLER__

.macro LL_CHECK_VALSTACK scratch
    leaq 8(%r12), \scratch
    cmpq ll_valstack_limit(%rip), \scratch
    ja ll_valstack_overflow
.endm

.macro LL_CHECK_RETSP scratch
    leaq LL_RETFRAME_SIZE(%r14), \scratch
    cmpq ll_retsp_limit(%rip), \scratch
    ja ll_retsp_overflow
.endm

#endif /* __ASSEMBLER__ */

#endif /* LL_STACKCHECK_H */
