# ll: AOT threaded-dispatch port of asnix. See
# ~/.claude/plans/soft-wishing-engelbart.md's M10 section for the
# design and what is/isn't vendored from asnix.
#
# M10: no LLVM code is linked into `ll` at runtime anymore. The op
# handlers (src/llvm/ops/*.c) are plain C, compiled once, ahead of
# time, into this binary -- the same role asnix's own hand-written
# asm handlers (src/asm/ops/*.S) play in asnix's own binary. clang is
# used (not gcc) specifically for its `[[clang::musttail]]` attribute
# (a hard, compile-time-checked guarantee that a handler-to-handler
# dispatch compiles to a real tail call, never a nested one) --
# that's the mechanism that keeps a deep Nix call/force chain from
# growing the native C stack, in place of asm's `jmp *%rax`. Every
# other file here is plain, portable C and would build equally well
# under gcc; using clang uniformly avoids a two-compiler build and
# matches the "convert ASM to LLVM, not JIT" thesis this port is
# testing (LLVM's own compiler producing the op handlers' machine
# code, ahead of time, rather than hand-written asm).
INC    := -Iinclude -Isrc/c -Isrc/llvm -Isrc/llvm/ops
CC     ?= clang
# FRONTEND_CC: the vendored frontend (src/c/*, byte-identical to
# asnix's own lexer/parser/compiler/builtins) has no musttail
# dependency at all -- it is ordinary, portable C. asnix's own
# Makefile compiles the identical files with `cc` (this devShell
# resolves that to gcc). Measured directly: compiling this same code
# with clang instead of gcc is a real, measurable slowdown for at
# least one workload (attrset_build_large's ~2,000-entry symbol
# sort), unrelated to anything this port's own dispatch-layer work
# controls -- using gcc here removes that confound rather than
# attributing it to the AOT dispatch redesign by mistake.
FRONTEND_CC ?= cc
OPT    ?= -O3
# -fomit-frame-pointer: every op handler ends in a musttail tail call
# (zero overhead already), but a handler with an internal fallback
# `call` (asnix_fatal/asnix_valstack_overflow on the cold path) still
# got a full `push %rbp; mov %rsp,%rbp`/`pop %rbp` frame-pointer
# convention around it by default -- pure overhead on the hot path,
# since nothing needs to walk frames through a tail-call chain that
# never grows the stack in the first place.
#
# -fno-zero-call-used-regs/-fno-stack-clash-protection: nixpkgs' own
# clang wrapper injects -fzero-call-used-regs=used-gpr and
# -fstack-clash-protection by default (confirmed via `clang -###`) --
# hardening mitigations against register-based info leaks and stack-
# clash attacks across a trust boundary. `ll` is a single-process,
# trusted-input evaluator with no such boundary on its own dispatch
# path; disassembly showed the register-zeroing specifically landing
# on every op handler's tail-call site (two extra `xor` instructions
# per op, real cost on the highest-repetition, lowest-work-per-op
# workloads this dispatch design is already most sensitive to -- see
# M10's own section below). Disabling both is a correctness-neutral,
# pure-performance choice for this project's actual threat model.
CFLAGS = -std=c11 -Wall -Wextra -g $(OPT) $(INC) -MMD -MP -U_FORTIFY_SOURCE -fno-stack-protector -fomit-frame-pointer -fzero-call-used-regs=skip -fno-stack-clash-protection -Wa,--noexecstack
LDFLAGS :=

MAKEFLAGS += -j$(shell nproc 2>/dev/null || echo 4)

ASM_SRCS := $(wildcard src/asm/*.S)
C_LIB_SRCS := $(wildcard src/c/*.c) \
              src/llvm/bytecode_cache_stub.c \
              src/llvm/env_helpers.c \
              src/llvm/closure_helpers.c \
              src/llvm/root_env.c \
              src/llvm/dispatch_table.c \
              src/llvm/ll_eval.c \
              src/llvm/ll_print.c \
              $(wildcard src/llvm/ops/*.c)
LIB_OBJS := $(ASM_SRCS:.S=.o) $(C_LIB_SRCS:.c=.o)
DEPS := $(LIB_OBJS:.o=.d)

.PHONY: all clean test
all: ll

test: ll
	bash tests/difftest.sh

ll: src/llvm/ll_main.o libll.a
	$(CC) $(LDFLAGS) -o $@ src/llvm/ll_main.o -L. -lll

libll.a: $(LIB_OBJS)
	ar rcs $@ $(LIB_OBJS)

src/asm/%.o: src/asm/%.S
	$(FRONTEND_CC) $(CFLAGS) -c -o $@ $<

src/c/%.o: src/c/%.c
	$(FRONTEND_CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f ll libll.a $(LIB_OBJS) $(DEPS) src/llvm/ll_main.o src/llvm/ll_main.d

-include $(DEPS)
-include src/llvm/ll_main.d
