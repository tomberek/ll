/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
// Shared "print a message and die" primitive, used by every abort path
// across the runtime (arena cap/mmap failures, M3 op type/arity errors) so
// the write+exit_group syscall sequence exists in exactly one place.
    .equ ASNIX_SYS_WRITE,      1
    .equ ASNIX_SYS_EXIT_GROUP, 231
    .equ ASNIX_STDERR_FD,      2

    .text
    .globl asnix_fatal
    .type asnix_fatal, @function
asnix_fatal:                            // void asnix_fatal(const char *msg, uint64_t len) -- never returns
    .cfi_startproc
    movq %rsi, %rdx                     // count = len
    movq %rdi, %rsi                     // buf = msg
    movq $ASNIX_STDERR_FD, %rdi
    movq $ASNIX_SYS_WRITE, %rax
    syscall
    movq $1, %rdi
    movq $ASNIX_SYS_EXIT_GROUP, %rax
    syscall
    // unreachable
    .cfi_endproc
    .size asnix_fatal, . - asnix_fatal
