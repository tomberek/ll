/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
// M1: value/tagging primitives. These are the C-callable, directly testable
// boundary -- the hot evaluator core (M3+) inlines these bit tricks directly
// in op handlers rather than paying a `call` per tag check; these routines
// exist for the C side (compiler, debug printing, tests) and as the proven
// reference implementation the inlined copies must match.
#include "ll/value.h"

    .section .rodata
.Lmsg_make_int_overflow:
    .ascii "ll: integer literal/result out of ll's representable range, aborting\n"
    .equ LL_MSG_MAKE_INT_OVERFLOW_LEN, . - .Lmsg_make_int_overflow

    .text

    /* M56: every C call site that builds a Value int (builtins.add/sub/
     * mul/div -- builtins_m23.c; fromTOML's own integer-literal parsing
     * -- toml.c; ll_add_values's int+int fallback -- value_util.c;
     * etc.) funnels through here, so checking the ±2^60 LL_INT_MIN/
     * MAX window once in this single reference implementation covers
     * all of them without duplicating the check at each call site. The
     * hot inlined-asm copies of this same bit trick (op_push_int,
     * op_arith.S's add/sub/mul/div) carry their own copy of this check
     * instead, per this file's own header comment on why they don't
     * just `call` this function. */
    .globl ll_make_int
    .type ll_make_int, @function
ll_make_int:                        // Value ll_make_int(int64_t n)
    .cfi_startproc
    movq %rdi, %rax
    movq $LL_INT_MAX, %r9
    cmpq %r9, %rax
    jg .Lmake_int_overflow
    movq $LL_INT_MIN, %r9
    cmpq %r9, %rax
    jl .Lmake_int_overflow
    salq $LL_TAG_BITS, %rax
    orq  $LL_TAG_INT, %rax
    ret
.Lmake_int_overflow:
    leaq .Lmsg_make_int_overflow(%rip), %rdi
    movq $LL_MSG_MAKE_INT_OVERFLOW_LEN, %rsi
    jmp ll_fatal
    .cfi_endproc
    .size ll_make_int, . - ll_make_int

    .globl ll_int_value
    .type ll_int_value, @function
ll_int_value:                       // int64_t ll_int_value(Value v)
    .cfi_startproc
    movq %rdi, %rax
    sarq $LL_TAG_BITS, %rax         // arithmetic shift: sign-preserving
    ret
    .cfi_endproc
    .size ll_int_value, . - ll_int_value

    .globl ll_make_imm
    .type ll_make_imm, @function
ll_make_imm:                        // Value ll_make_imm(int subtag)
    .cfi_startproc
    movl %edi, %eax                    // zero-extends into rax; the SysV ABI
                                        // leaves rdi's upper 32 bits undefined
                                        // for a 32-bit int argument
    salq $LL_TAG_BITS, %rax
    orq  $LL_TAG_IMM, %rax
    ret
    .cfi_endproc
    .size ll_make_imm, . - ll_make_imm

    .globl ll_imm_subtag
    .type ll_imm_subtag, @function
ll_imm_subtag:                      // int ll_imm_subtag(Value v)
    .cfi_startproc
    movq %rdi, %rax
    shrq $LL_TAG_BITS, %rax         // subtag is always non-negative
    ret
    .cfi_endproc
    .size ll_imm_subtag, . - ll_imm_subtag

    .globl ll_tag_of
    .type ll_tag_of, @function
ll_tag_of:                          // int ll_tag_of(Value v)
    .cfi_startproc
    movq %rdi, %rax
    andq $LL_TAG_MASK, %rax
    ret
    .cfi_endproc
    .size ll_tag_of, . - ll_tag_of

    .globl ll_is_thunk
    .type ll_is_thunk, @function
ll_is_thunk:                        // int ll_is_thunk(Value v)
    .cfi_startproc
    movq %rdi, %rax
    andq $LL_TAG_MASK, %rax
    cmpq $LL_TAG_THUNK, %rax
    sete %al
    movzbl %al, %eax
    ret
    .cfi_endproc
    .size ll_is_thunk, . - ll_is_thunk
