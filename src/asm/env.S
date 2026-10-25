/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
// M3: Env frame allocation and De Bruijn lookup. Tested, C-callable
// primitives -- op_var_ref/op_let/op_call inline the same logic directly
// in the hot dispatch path rather than paying a `call` per use (same
// precedent as src/asm/value.S).
#include "asnix/env.h"

    .text

    .globl asnix_env_alloc
    .type asnix_env_alloc, @function
asnix_env_alloc:                        // Env *asnix_env_alloc(Env *up, int64_t nslots)
    .cfi_startproc
    pushq %rbx
    pushq %rbp
    movq %rdi, %rbx                     // save `up` across the call (rdi becomes the size arg)
    movq %rsi, %rbp                     // save nslots across the call, for stats accounting
    leaq ASNIX_ENV_OFF_VALUES(, %rsi, 8), %rdi // size = 4 (up) + nslots*8
    call asnix_env_arena_alloc
    incq asnix_stats_env(%rip)
    addq %rbp, asnix_stats_env_elements(%rip)
    // M118: compress `up` -- NULL-safe, unlike the hot-path-inlined
    // allocation sites (op_frame.S/op_let.S/op_with.S), which only ever
    // see a live, non-NULL current ENV and skip this check; this is a
    // general-purpose entry point genuinely called with up=NULL (every
    // root Env, see eval_file.c/builtins_shared.c/builtins_attrs.c).
    testq %rbx, %rbx
    jz .Lenv_alloc_up_null
    subq asnix_env_arena_base(%rip), %rbx
.Lenv_alloc_up_null:
    movl %ebx, ASNIX_ENV_OFF_UP(%rax)
    popq %rbp
    popq %rbx
    ret
    .cfi_endproc
    .size asnix_env_alloc, . - asnix_env_alloc

    .globl asnix_env_lookup
    .type asnix_env_lookup, @function
asnix_env_lookup:                       // Value asnix_env_lookup(Env *env, int64_t depth, int64_t slot)
    .cfi_startproc
.Lenv_lookup_walk:
    testq %rsi, %rsi
    jz .Lenv_lookup_done
    movl ASNIX_ENV_OFF_UP(%rdi), %edi   // M118: zero-extends into %rdi; decompress below
    addq asnix_env_arena_base(%rip), %rdi
    andq $-2, %rdi                      // mask off ASNIX_ENV_WITH_TAG in case this hop is a with-frame
    decq %rsi
    jmp .Lenv_lookup_walk
.Lenv_lookup_done:
    movq ASNIX_ENV_OFF_VALUES(%rdi, %rdx, 8), %rax
    ret
    .cfi_endproc
    .size asnix_env_lookup, . - asnix_env_lookup
