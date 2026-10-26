/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
// M118: dedicated, bounded arena for Env objects only, enabling Env.up to
// be stored as a 32-bit offset instead of a 64-bit pointer (env.h).
// Deliberately SEPARATE from the general, unbounded asnix_arena
// (arena.S): a compressed offset is only valid if every object it can
// ever address lives inside ONE bounded, contiguous region, and the
// general arena's repeated, address-unconstrained mmap() growth (its
// .Lgrow path) gives no such guarantee -- successive chunks can land
// anywhere, with no promise of contiguity between them. This arena
// never grows past its single upfront reservation: a hard, generously-
// sized cap (ASNIX_ENV_ARENA_SIZE, env.h), not a soft/advisory limit.
//
// Unlike the general arena, this one does NOT round allocations up to
// the next multiple of 8: Env is never itself a tagged Value (unlike
// every OTHER heap kind that goes through asnix_arena_alloc), so it
// never needs Value.raw's 3 free low tag bits -- the only thing that
// ever needs ANY free low bits on an Env address is env.h's own
// ASNIX_ENV_WITH_TAG (one bit). Rounding to 4 instead of 8 is what lets
// Env.up's compression actually shrink Env's total allocated size
// instead of being clawed straight back by rounding, the same way a
// naive reuse of the general arena's own round_up_8 would (see
// docs/architecture.md's M118 section for the exact arithmetic -- this
// is the fix for the blind spot M117's first pass missed).
//
// Lazily self-initializes on first call (checked via the bump cursor
// being exactly 0, which it only ever is before the one-time mmap) so
// no caller anywhere -- production or test -- needs to remember to
// call an explicit init function before the first Env allocation.
#include "asnix/env.h"

    .equ ASNIX_SYS_MMAP,       9
    .equ ASNIX_PROT_READ,      0x1
    .equ ASNIX_PROT_WRITE,     0x2
    .equ ASNIX_MAP_PRIVATE,    0x02
    .equ ASNIX_MAP_ANONYMOUS,  0x20
    .equ ASNIX_MAP_NORESERVE,  0x4000

    .bss
    .align 8
    .globl asnix_env_arena_base
    .type asnix_env_arena_base, @object
asnix_env_arena_base:
    .quad 0
    .size asnix_env_arena_base, 8

    .align 8
    // M8 perf work: exported (.globl) so both op_dispatch.h's ll_env_alloc
    // and closure_helpers.c's ll_env_alloc_fast can inline this allocator's
    // bump-pointer fast path directly from C (same precedent as arena.h's
    // AsnixArenaState.cur/end, which ll_arena_alloc already reads this way)
    // -- previously file-local, since only asnix_env_arena_alloc itself
    // (this file) needed them. Purely additive; asnix's own asm callers
    // are unaffected.
    .globl asnix_env_arena_cur
    .type asnix_env_arena_cur, @object
asnix_env_arena_cur:
    .quad 0                              // 0 until the first-ever alloc -- doubles as the
                                          // lazy-init check below
    .size asnix_env_arena_cur, 8

    .align 8
    .globl asnix_env_arena_end
    .type asnix_env_arena_end, @object
asnix_env_arena_end:
    .quad 0
    .size asnix_env_arena_end, 8

    .section .rodata
.Lmsg_mmap:
    .ascii "asnix: mmap failed while reserving the Env arena, aborting\n"
    .equ ASNIX_MSG_MMAP_LEN, . - .Lmsg_mmap
.Lmsg_cap:
    .ascii "asnix: Env arena exhausted (ASNIX_ENV_ARENA_SIZE), aborting\n"
    .equ ASNIX_MSG_CAP_LEN, . - .Lmsg_cap

    .text

    .globl asnix_env_arena_alloc
    .type asnix_env_arena_alloc, @function
asnix_env_arena_alloc:                  // void *asnix_env_arena_alloc(uint64_t n) -- round_up_4, bounded
    .cfi_startproc
    leaq asnix_env_arena_cur(%rip), %rax
    movq (%rax), %rcx
    testq %rcx, %rcx
    jnz .Lenv_arena_ready

    pushq %rdi                          // save n across the one-time lazy-init call
    call asnix_env_arena_do_init
    popq %rdi

.Lenv_arena_ready:
    addq $3, %rdi
    andq $-4, %rdi                      // n4 = round_up_4(n)
    leaq asnix_env_arena_cur(%rip), %rax
    movq (%rax), %rcx                   // rcx = cur (re-read: do_init may have just set it)
    leaq (%rcx, %rdi), %rdx             // rdx = cur + n4
    leaq asnix_env_arena_end(%rip), %rsi
    cmpq (%rsi), %rdx
    ja .Lenv_arena_abort_cap
    movq %rdx, (%rax)                   // cur = cur + n4
    movq %rcx, %rax                     // return old cur
    ret
.Lenv_arena_abort_cap:
    leaq .Lmsg_cap(%rip), %rdi
    movq $ASNIX_MSG_CAP_LEN, %rsi
    jmp asnix_fatal                     // never returns; no stack cleanup needed
    .cfi_endproc
    .size asnix_env_arena_alloc, . - asnix_env_arena_alloc

    .type asnix_env_arena_do_init, @function
asnix_env_arena_do_init:                // internal, called exactly once (from asnix_env_arena_alloc)
    .cfi_startproc
    // mmap(NULL, ASNIX_ENV_ARENA_SIZE, PROT_READ|WRITE, MAP_PRIVATE|ANON|NORESERVE, -1, 0)
    xorq %rdi, %rdi
    movq $ASNIX_ENV_ARENA_SIZE, %rsi
    movq $(ASNIX_PROT_READ | ASNIX_PROT_WRITE), %rdx
    movq $(ASNIX_MAP_PRIVATE | ASNIX_MAP_ANONYMOUS | ASNIX_MAP_NORESERVE), %r10
    movq $-1, %r8
    xorq %r9, %r9
    movq $ASNIX_SYS_MMAP, %rax
    syscall
    testq %rax, %rax
    jl .Lenv_arena_abort_mmap

    leaq asnix_env_arena_base(%rip), %rcx
    movq %rax, (%rcx)
    // cur starts at base+8, not base+0, so a stored offset of 0 can
    // always mean NULL unambiguously, never a real (if coincidental)
    // allocation at the arena's very first byte.
    addq $8, %rax
    leaq asnix_env_arena_cur(%rip), %rcx
    movq %rax, (%rcx)
    subq $8, %rax
    addq $ASNIX_ENV_ARENA_SIZE, %rax
    leaq asnix_env_arena_end(%rip), %rcx
    movq %rax, (%rcx)
    ret
.Lenv_arena_abort_mmap:
    leaq .Lmsg_mmap(%rip), %rdi
    movq $ASNIX_MSG_MMAP_LEN, %rsi
    jmp asnix_fatal                     // never returns; no stack cleanup needed
    .cfi_endproc
    .size asnix_env_arena_do_init, . - asnix_env_arena_do_init
