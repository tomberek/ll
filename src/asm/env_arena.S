/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
// Dedicated, bounded arena for Env objects only, enabling Env.up to
// be stored as a 32-bit offset instead of a 64-bit pointer (env.h).
// Deliberately SEPARATE from the general, unbounded ll_arena
// (arena.S): a compressed offset is only valid if every object it can
// ever address lives inside ONE bounded, contiguous region, and the
// general arena's repeated, address-unconstrained mmap() growth (its
// .Lgrow path) gives no such guarantee -- successive chunks can land
// anywhere, with no promise of contiguity between them. This arena
// never grows past its single upfront reservation: a hard, generously-
// sized cap (LL_ENV_ARENA_SIZE, env.h), not a soft/advisory limit.
//
// Unlike the general arena, this one does NOT round allocations up to
// the next multiple of 8: Env is never itself a tagged Value (unlike
// every OTHER heap kind that goes through ll_arena_alloc), so it
// never needs Value.raw's 3 free low tag bits -- the only thing that
// ever needs ANY free low bits on an Env address is env.h's own
// LL_ENV_WITH_TAG (one bit).
//
// Lazily self-initializes on first call (checked via the bump cursor
// being exactly 0, which it only ever is before the one-time mmap).
#include "ll/arena.h"
#include "ll/env.h"

    .equ LL_SYS_MMAP,       9
    .equ LL_SYS_MADVISE,    28
    .equ LL_MADV_HUGEPAGE,  14
    .equ LL_PROT_READ,      0x1
    .equ LL_PROT_WRITE,     0x2
    .equ LL_MAP_PRIVATE,    0x02
    .equ LL_MAP_ANONYMOUS,  0x20
    .equ LL_MAP_NORESERVE,  0x4000

    .bss
    .align 8
    .globl ll_env_arena_base
    .type ll_env_arena_base, @object
ll_env_arena_base:
    .quad 0
    .size ll_env_arena_base, 8

    .align 8
    // Exported (.globl) so op_dispatch.h's ll_env_alloc and
    // closure_helpers.c's ll_env_alloc_fast can inline this allocator's
    // bump-pointer fast path directly from C.
    .globl ll_env_arena_cur
    .type ll_env_arena_cur, @object
ll_env_arena_cur:
    .quad 0                              // 0 until the first-ever alloc -- doubles as the
                                          // lazy-init check below
    .size ll_env_arena_cur, 8

    .align 8
    .globl ll_env_arena_end
    .type ll_env_arena_end, @object
ll_env_arena_end:
    .quad 0
    .size ll_env_arena_end, 8

    .section .rodata
.Lmsg_mmap:
    .ascii "ll: mmap failed while reserving the Env arena, aborting\n"
    .equ LL_MSG_MMAP_LEN, . - .Lmsg_mmap
.Lmsg_cap:
    .ascii "ll: Env arena exhausted (LL_ENV_ARENA_SIZE), aborting\n"
    .equ LL_MSG_CAP_LEN, . - .Lmsg_cap

    .text

    .globl ll_env_arena_alloc
    .type ll_env_arena_alloc, @function
ll_env_arena_alloc:                  // void *ll_env_arena_alloc(uint64_t n) -- round_up_4, bounded
    .cfi_startproc
    leaq ll_env_arena_cur(%rip), %rax
    movq (%rax), %rcx
    testq %rcx, %rcx
    jnz .Lenv_arena_ready

    pushq %rdi                          // save n across the one-time lazy-init call
    call ll_env_arena_do_init
    popq %rdi

.Lenv_arena_ready:
    addq $3, %rdi
    andq $-4, %rdi                      // n4 = round_up_4(n)
    leaq ll_env_arena_cur(%rip), %rax
    movq (%rax), %rcx                   // rcx = cur (re-read: do_init may have just set it)
    leaq (%rcx, %rdi), %rdx             // rdx = cur + n4
    leaq ll_env_arena_end(%rip), %rsi
    cmpq (%rsi), %rdx
    ja .Lenv_arena_abort_cap
    movq %rdx, (%rax)                   // cur = cur + n4
    movq %rcx, %rax                     // return old cur
    ret
.Lenv_arena_abort_cap:
    leaq .Lmsg_cap(%rip), %rdi
    movq $LL_MSG_CAP_LEN, %rsi
    jmp ll_fatal                     // never returns; no stack cleanup needed
    .cfi_endproc
    .size ll_env_arena_alloc, . - ll_env_arena_alloc

    .type ll_env_arena_do_init, @function
ll_env_arena_do_init:                // internal, called exactly once (from ll_env_arena_alloc)
    .cfi_startproc
    // mmap(NULL, LL_ENV_ARENA_SIZE, PROT_READ|WRITE, MAP_PRIVATE|ANON|NORESERVE, -1, 0)
    xorq %rdi, %rdi
    movq $LL_ENV_ARENA_SIZE, %rsi
    movq $(LL_PROT_READ | LL_PROT_WRITE), %rdx
    movq $(LL_MAP_PRIVATE | LL_MAP_ANONYMOUS | LL_MAP_NORESERVE), %r10
    movq $-1, %r8
    xorq %r9, %r9
    movq $LL_SYS_MMAP, %rax
    syscall
    testq %rax, %rax
    jl .Lenv_arena_abort_mmap

    // ll_huge_pages_available() -- gate the hint below on this, same
    // as arena.S's general-arena fix. rax (the mmap result) must
    // survive the call.
    pushq %rax
    call ll_huge_pages_available
    popq %rax
    testl %eax, %eax
    jz .Lno_env_hugepage_hint

    // madvise(base, LL_ENV_ARENA_SIZE, MADV_HUGEPAGE) -- same hint as
    // arena.S's general-arena fix. Return value ignored (hint only);
    // rax (the mmap result, needed right below) must survive the
    // syscall.
    pushq %rax
    movq %rax, %rdi
    movq $LL_ENV_ARENA_SIZE, %rsi
    movq $LL_MADV_HUGEPAGE, %rdx
    movq $LL_SYS_MADVISE, %rax
    syscall
    popq %rax

.Lno_env_hugepage_hint:
    leaq ll_env_arena_base(%rip), %rcx
    movq %rax, (%rcx)
    // cur starts at base+8, not base+0, so a stored offset of 0 can
    // always mean NULL unambiguously, never a real (if coincidental)
    // allocation at the arena's very first byte.
    addq $8, %rax
    leaq ll_env_arena_cur(%rip), %rcx
    movq %rax, (%rcx)
    subq $8, %rax
    addq $LL_ENV_ARENA_SIZE, %rax
    leaq ll_env_arena_end(%rip), %rcx
    movq %rax, (%rcx)
    ret
.Lenv_arena_abort_mmap:
    leaq .Lmsg_mmap(%rip), %rdi
    movq $LL_MSG_MMAP_LEN, %rsi
    jmp ll_fatal                     // never returns; no stack cleanup needed
    .cfi_endproc
    .size ll_env_arena_do_init, . - ll_env_arena_do_init
