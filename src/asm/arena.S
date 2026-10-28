/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
// M2: bump arena allocator. Never-free: nothing handed out by
// asnix_arena_alloc is ever individually freed. Uses raw Linux x86-64
// syscalls directly (no libc dependency) since this is core runtime, not a
// C-boundary routine.
#include "asnix/arena.h"

    .equ ASNIX_SYS_MMAP,       9
    .equ ASNIX_SYS_MADVISE,    28
    .equ ASNIX_MADV_HUGEPAGE,  14
    .equ ASNIX_MADV_POPULATE_WRITE, 23
    .equ ASNIX_PROT_READ,      0x1
    .equ ASNIX_PROT_WRITE,     0x2
    .equ ASNIX_MAP_PRIVATE,    0x02
    .equ ASNIX_MAP_ANONYMOUS,  0x20
    .equ ASNIX_PAGE_SIZE,      0x1000

    .bss
    .align 8
    .globl asnix_arena
    .type asnix_arena, @object
asnix_arena:
    .quad 0 // cur
    .quad 0 // end
    .quad 0 // total_allocated
    .quad 0 // max_memory
    .size asnix_arena, 32

    .section .rodata
.Lmsg_cap:
    .ascii "asnix: arena memory cap (ASNIX_MAX_MEMORY) exceeded, aborting\n"
    .equ ASNIX_MSG_CAP_LEN, . - .Lmsg_cap
.Lmsg_mmap:
    .ascii "asnix: mmap failed while growing the bump arena, aborting\n"
    .equ ASNIX_MSG_MMAP_LEN, . - .Lmsg_mmap

    .text

    .globl asnix_arena_init
    .type asnix_arena_init, @function
asnix_arena_init:                       // void asnix_arena_init(uint64_t max_memory_bytes)
    .cfi_startproc
    leaq asnix_arena(%rip), %rax
    movq $0, ASNIX_ARENA_OFF_CUR(%rax)
    movq $0, ASNIX_ARENA_OFF_END(%rax)
    movq $0, ASNIX_ARENA_OFF_TOTAL(%rax)
    movq %rdi, ASNIX_ARENA_OFF_MAX(%rax)
    ret
    .cfi_endproc
    .size asnix_arena_init, . - asnix_arena_init

    .globl asnix_arena_alloc
    .type asnix_arena_alloc, @function
asnix_arena_alloc:                      // void *asnix_arena_alloc(uint64_t n)
    .cfi_startproc
    pushq %rbx                          // callee-saved; we use it to hold n8 across this call
    addq $7, %rdi
    andq $-8, %rdi                      // %rdi = n8 = round_up_8(n)
    movq %rdi, %rbx

    leaq asnix_arena(%rip), %rax
    movq ASNIX_ARENA_OFF_MAX(%rax), %rcx
    testq %rcx, %rcx
    jz .Lno_cap_check                   // max_memory == 0 => uncapped
    movq ASNIX_ARENA_OFF_TOTAL(%rax), %rdx
    addq %rbx, %rdx
    cmpq %rcx, %rdx
    jbe .Lno_cap_check                  // total_allocated + n8 <= max_memory => ok
    jmp .Labort_cap

.Lno_cap_check:
    movq ASNIX_ARENA_OFF_CUR(%rax), %rcx // rcx = cur
    movq ASNIX_ARENA_OFF_END(%rax), %rdx // rdx = end
    leaq (%rcx, %rbx), %rsi               // rsi = cur + n8 (candidate new cur)
    cmpq %rdx, %rsi
    ja .Lgrow                            // cur + n8 > end => current chunk doesn't fit

    // Fast path: the allocation fits in the current chunk.
    movq %rsi, ASNIX_ARENA_OFF_CUR(%rax)
    movq ASNIX_ARENA_OFF_TOTAL(%rax), %rdx
    addq %rbx, %rdx
    movq %rdx, ASNIX_ARENA_OFF_TOTAL(%rax)
    movq %rcx, %rax                      // return pointer = old cur
    popq %rbx
    ret

.Lgrow:
    // chunk_size = max(round_up_page(n8), ASNIX_ARENA_CHUNK_SIZE)
    movq %rbx, %r8
    addq $(ASNIX_PAGE_SIZE - 1), %r8
    andq $-ASNIX_PAGE_SIZE, %r8
    movq $ASNIX_ARENA_CHUNK_SIZE, %r9
    cmpq %r9, %r8
    cmovbe %r9, %r8                      // r8 = chunk_size

    pushq %r8                            // save chunk_size across the syscall

    // mmap(NULL, chunk_size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
    xorq %rdi, %rdi
    movq %r8, %rsi
    movq $(ASNIX_PROT_READ | ASNIX_PROT_WRITE), %rdx
    movq $(ASNIX_MAP_PRIVATE | ASNIX_MAP_ANONYMOUS), %r10
    movq $-1, %r8
    xorq %r9, %r9
    movq $ASNIX_SYS_MMAP, %rax
    syscall
    testq %rax, %rax
    jl .Labort_mmap                      // raw syscall error: rax = -errno (always negative)

    popq %r8                             // r8 = chunk_size (pushed before the mmap syscall)
    // madvise(new_chunk, chunk_size, MADV_HUGEPAGE) -- hint only, return
    // value ignored: this kernel's transparent_hugepage/enabled is
    // "madvise", not "always" (confirmed via /sys), so ASNIX_ARENA_CHUNK_
    // SIZE's own 2 MiB chunks -- exactly one x86-64 huge page -- silently
    // stay 512 separate 4 KiB pages, each its own minor page fault on
    // first touch, unless explicitly hinted here. A real, measured
    // contributor to this evaluator's much higher page-fault count vs
    // iets (every single heap object funnels through this one bump
    // allocator). Failure (no THP support at all) just forgoes the
    // speedup, never a correctness problem, so the return value is never
    // checked. rax currently holds new_chunk (the mmap result); both it
    // and chunk_size (r8) must survive the syscall's own clobbers.
    pushq %rax
    pushq %r8
    movq %rax, %rdi
    movq %r8, %rsi
    movq $ASNIX_MADV_HUGEPAGE, %rdx
    movq $ASNIX_SYS_MADVISE, %rax
    syscall

    // madvise(new_chunk, chunk_size, MADV_POPULATE_WRITE) -- the hint
    // above is not enough on its own: this kernel's transparent_
    // hugepage/defrag is "madvise", meaning huge-page promotion for a
    // MADV_HUGEPAGE region is attempted via DIRECT (synchronous)
    // compaction at first-touch fault time, one lazy 4 KiB fault at a
    // time as the bump cursor advances -- unreliable for actually
    // landing a huge page, and (per iets's own sys/linux.rs comment,
    // read directly rather than guessed) this exact "many threads
    // compacting at once on a fragmented box" pattern was a measured
    // regression for them. Eagerly populating the WHOLE chunk now, in
    // one syscall, gives the kernel the full 2 MiB extent up front
    // instead of discovering it fault-by-fault, which is what actually
    // makes promotion land reliably. Still hint-only (return value
    // ignored) -- iets additionally gates this on free order-9+ blocks
    // in /proc/buddyinfo before ever requesting a huge page at all,
    // avoiding the fragmented-box regression at its root; NOT ported
    // here (would need a C-side one-time check, this file has no libc
    // dependency by design) -- a known, deliberate scope cut, safe on
    // this box only because /proc/buddyinfo currently shows hundreds of
    // MB free at order 9/10. Revisit if this ever regresses instead of
    // helping. %rdi/%rsi are already new_chunk/chunk_size from above.
    movq $ASNIX_MADV_POPULATE_WRITE, %rdx
    movq $ASNIX_SYS_MADVISE, %rax
    syscall
    popq %r8                             // restore chunk_size
    popq %rax                            // restore new_chunk pointer

    leaq asnix_arena(%rip), %rcx
    leaq (%rax, %r8), %rdx
    movq %rdx, ASNIX_ARENA_OFF_END(%rcx) // end = new_chunk + chunk_size

    movq %rax, %rsi                      // rsi = result pointer = new_chunk
    addq %rbx, %rax                      // rax = new_chunk + n8
    movq %rax, ASNIX_ARENA_OFF_CUR(%rcx)
    movq ASNIX_ARENA_OFF_TOTAL(%rcx), %rdx
    addq %rbx, %rdx
    movq %rdx, ASNIX_ARENA_OFF_TOTAL(%rcx)

    movq %rsi, %rax                      // return pointer
    popq %rbx
    ret

.Labort_mmap:
    leaq .Lmsg_mmap(%rip), %rdi
    movq $ASNIX_MSG_MMAP_LEN, %rsi
    jmp asnix_fatal                      // never returns; no stack cleanup needed

.Labort_cap:
    leaq .Lmsg_cap(%rip), %rdi
    movq $ASNIX_MSG_CAP_LEN, %rsi
    jmp asnix_fatal                      // never returns; no stack cleanup needed
    .cfi_endproc
    .size asnix_arena_alloc, . - asnix_arena_alloc
