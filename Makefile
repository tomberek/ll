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

# M146 port (from asnix, never ported here before now): statically
# link mimalloc into the final `ll` executable as a process-wide
# override of malloc/free/calloc/realloc -- same mechanism, same
# "no source change needed" proof (mimalloc's static archive already
# exports standard-named malloc/free/calloc/realloc symbols) asnix's
# own Makefile already uses in production. This project's own
# flake.nix already exports `ASNIX_MIMALLOC_LIB` for exactly this
# (shellHook sets it, matching asnix's own devShell convention) --
# this Makefile just never consumed it. Falls back to a portable
# system-mimalloc link probe outside the flake's devShell, then to
# plain glibc malloc if neither is available -- never a hard build
# failure either way.
ifdef ASNIX_MIMALLOC_LIB
MIMALLOC_LDLIBS := $(ASNIX_MIMALLOC_LIB) -lpthread
else
MIMALLOC_PROBE := $(shell printf 'int main(void){return 0;}' | $(CC) -xc - -lmimalloc -o /dev/null 2>/dev/null && echo yes)
ifeq ($(MIMALLOC_PROBE),yes)
MIMALLOC_LDLIBS := -lmimalloc -lpthread
endif
endif

MAKEFLAGS += -j$(shell nproc 2>/dev/null || echo 4)

ASM_SRCS := $(wildcard src/asm/*.S)
C_LIB_SRCS := $(wildcard src/c/*.c) \
              src/llvm/env_helpers.c \
              src/llvm/closure_helpers.c \
              src/llvm/root_env.c \
              src/llvm/dispatch_table.c \
              src/llvm/ll_eval.c \
              src/llvm/ll_print.c \
              $(wildcard src/llvm/ops/*.c)
LIB_OBJS := $(ASM_SRCS:.S=.o) $(C_LIB_SRCS:.c=.o)
DEPS := $(LIB_OBJS:.o=.d)

.PHONY: all clean test test-nix-conformance
all: ll

test: ll
	bash tests/difftest.sh

# Runs the real github.com/numtide/nix-conformance corpus through
# tests/nix-conformance-adapter, via that project's own `run` script (not
# a reimplementation of its comparison logic -- see the adapter's own
# header comment for the protocol). Clone the corpus to
# NIX_CONFORMANCE_DIR first (default /tmp/nix-conformance), or pass
# NIX_CONFORMANCE_DIR=/path/to/it. tests/nix-conformance-known-failures.txt
# lists the (currently 3) cases this evaluator fails on purpose -- `run
# --known` counts those as "known" rather than "fail" and, conversely,
# fails the build if one of them starts PASSING (a sign the list is
# stale), so this target's exit code is a real pass/fail gate, suitable
# for CI, unlike test-nixpkgs-hash's deliberately non-gating sweep.
NIX_CONFORMANCE_DIR ?= /tmp/nix-conformance
test-nix-conformance: ll
	@if [ ! -d "$(NIX_CONFORMANCE_DIR)/lang" ]; then \
		echo "test-nix-conformance: '$(NIX_CONFORMANCE_DIR)/lang' not found -- clone" \
		     "https://github.com/numtide/nix-conformance to $(NIX_CONFORMANCE_DIR)" \
		     "(or pass NIX_CONFORMANCE_DIR=/path/to/it) first" >&2; \
		exit 1; \
	fi
	LL_BIN=$(CURDIR)/ll "$(NIX_CONFORMANCE_DIR)/run" \
		--known $(CURDIR)/tests/nix-conformance-known-failures.txt \
		$(CURDIR)/tests/nix-conformance-adapter lang

ll: src/llvm/ll_main.o libll.a
	$(CC) $(LDFLAGS) -o $@ src/llvm/ll_main.o -L. -lll $(MIMALLOC_LDLIBS)

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

# --- Profile-Guided Optimization (PGO): opt-in, NOT part of `make ll`/`all` ---
#
# Why opt-in: profile data goes stale as code changes, and baking a
# multi-step, training-data-dependent pipeline into the default build is
# the wrong default for a project still under active development. Run
# `make pgo` by hand (and re-run it after any change to src/llvm/** worth
# re-training on); it is not wired into `all`/`test`/CI.
#
# What gets instrumented/optimized: ONLY the clang-compiled files that
# $(CC) builds under the generic `%.o: %.c`/`%.o: %.S` rules above
# (src/llvm/*.c, src/llvm/ops/*.c, src/llvm/ll_main.c) -- i.e. exactly the
# files `-fprofile-instr-generate`/`-fprofile-instr-use` can apply to in
# the first place. The vendored frontend (src/c/*.c, FRONTEND_CC/gcc) and
# the asm shims (src/asm/*.S, also built with FRONTEND_CC) don't go
# through clang's `-fprofile-instr-*` machinery at all -- gcc doesn't
# understand those flags -- so they're compiled identically to the normal
# `make ll` build in both PGO passes below, just mirrored into their own
# object directories so `make ll` and `make pgo` never clobber each
# other's .o files.
#
# Pipeline (standard clang PGO recipe), all driven by `make pgo`:
#   1. pgo-instrumented: build $(PGO_GEN_BIN) with -fprofile-instr-generate
#   2. pgo-train:        run it over the training set (bench/workloads/*.nix
#                         + a fixed, deliberately-mixed set of real nixpkgs
#                         attrs -- see PGO_TRAIN_ATTRS below), each run
#                         writing its own *.profraw via LLVM_PROFILE_FILE's
#                         %p (pid)/%m (per-binary signature) so concurrent
#                         runs never clobber each other
#   3. pgo-merge:         llvm-profdata merge *.profraw -> merged.profdata
#   4. pgo-build:         rebuild $(PGO_BIN) with -fprofile-instr-use=...
#
# Training set is deliberately NOT ffmpeg-full.outPath -- that is this
# project's held-out correctness/performance target; training on it would
# make any speedup an overfitting artifact rather than a real result. See
# bench/results/M8-findings.md for how $(PGO_BIN) compared to plain `ll`.
PGO_DIR        := .pgo
PGO_RAW_DIR    := $(PGO_DIR)/profraw
PGO_PROFDATA   := $(PGO_DIR)/merged.profdata
PGO_GEN_OBJDIR := $(PGO_DIR)/obj-gen
PGO_USE_OBJDIR := $(PGO_DIR)/obj-use
PGO_GEN_BIN    := ll-pgo-gen
PGO_BIN        := ll-pgo

LLVM_PROFDATA  ?= llvm-profdata

PGO_GEN_FLAG   := -fprofile-instr-generate
PGO_USE_FLAG   := -fprofile-instr-use=$(PGO_PROFDATA)

PGO_GEN_ASM_OBJS := $(addprefix $(PGO_GEN_OBJDIR)/,$(ASM_SRCS:.S=.o))
PGO_GEN_C_OBJS   := $(addprefix $(PGO_GEN_OBJDIR)/,$(C_LIB_SRCS:.c=.o))
PGO_GEN_MAIN_OBJ := $(PGO_GEN_OBJDIR)/src/llvm/ll_main.o
PGO_GEN_OBJS     := $(PGO_GEN_ASM_OBJS) $(PGO_GEN_C_OBJS) $(PGO_GEN_MAIN_OBJ)
PGO_GEN_DEPS     := $(PGO_GEN_OBJS:.o=.d)

PGO_USE_ASM_OBJS := $(addprefix $(PGO_USE_OBJDIR)/,$(ASM_SRCS:.S=.o))
PGO_USE_C_OBJS   := $(addprefix $(PGO_USE_OBJDIR)/,$(C_LIB_SRCS:.c=.o))
PGO_USE_MAIN_OBJ := $(PGO_USE_OBJDIR)/src/llvm/ll_main.o
PGO_USE_OBJS     := $(PGO_USE_ASM_OBJS) $(PGO_USE_C_OBJS) $(PGO_USE_MAIN_OBJ)
PGO_USE_DEPS     := $(PGO_USE_OBJS:.o=.d)

# Training workloads: all ten synthetic microbenchmarks, plus a fixed,
# deliberately-mixed (small/medium/large, different domains) set of real
# nixpkgs attrs, evaluated the same way tests/nixpkgs-hash's correctness
# sweep does: `let pkgs = import /home/tbereknyei/nixpkgs/default.nix {};
# in pkgs.<attr>.outPath`. Deliberately excludes ffmpeg-full (see above).
PGO_TRAIN_WORKLOADS := $(wildcard bench/workloads/*.nix)
PGO_TRAIN_ATTRS := hello coreutils git cmake openssl glib boost python3 nodejs gcc

.PHONY: pgo pgo-instrumented pgo-train pgo-merge pgo-build pgo-clean pgo-train-clean

pgo: pgo-build

pgo-instrumented: $(PGO_GEN_BIN)

$(PGO_GEN_BIN): $(PGO_GEN_OBJS)
	$(CC) $(LDFLAGS) $(PGO_GEN_FLAG) -o $@ $(PGO_GEN_OBJS)

# frontend/asm objects (FRONTEND_CC): same flags as `make ll`'s own
# src/asm/%.o and src/c/%.o rules, just mirrored into $(PGO_GEN_OBJDIR).
$(PGO_GEN_OBJDIR)/src/asm/%.o: src/asm/%.S
	@mkdir -p $(dir $@)
	$(FRONTEND_CC) $(CFLAGS) -c -o $@ $<

$(PGO_GEN_OBJDIR)/src/c/%.o: src/c/%.c
	@mkdir -p $(dir $@)
	$(FRONTEND_CC) $(CFLAGS) -c -o $@ $<

# clang-compiled objects: these get instrumented.
$(PGO_GEN_OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(PGO_GEN_FLAG) -c -o $@ $<

$(PGO_GEN_OBJDIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(PGO_GEN_FLAG) -c -o $@ $<

# Step 2: train. LLVM_PROFILE_FILE's %p/%m let every run (one per
# workload/attr) write its own *.profraw instead of overwriting a single
# default.profraw -- see `clang -cc1 --help`'s -fprofile-instrument-path.
pgo-train: $(PGO_GEN_BIN)
	@mkdir -p $(PGO_RAW_DIR)
	@rm -f $(PGO_RAW_DIR)/*.profraw
	@for f in $(PGO_TRAIN_WORKLOADS); do \
		echo "pgo-train: $$f"; \
		LLVM_PROFILE_FILE="$(PGO_RAW_DIR)/%p-%m.profraw" ./$(PGO_GEN_BIN) "$$f" >/dev/null 2>&1 || \
			echo "pgo-train: WARNING: $$f exited nonzero, profile for this run may be partial/missing"; \
	done
	@for a in $(PGO_TRAIN_ATTRS); do \
		echo "pgo-train: pkgs.$$a.outPath"; \
		LLVM_PROFILE_FILE="$(PGO_RAW_DIR)/%p-%m.profraw" ./$(PGO_GEN_BIN) \
			"let pkgs = import /home/tbereknyei/nixpkgs/default.nix {}; in pkgs.$$a.outPath" >/dev/null 2>&1 || \
			echo "pgo-train: WARNING: pkgs.$$a.outPath exited nonzero, profile for this run may be partial/missing"; \
	done

# Step 3: merge. $(PGO_PROFDATA) is a real file target (not phony) so the
# $(PGO_USE_OBJDIR)/%.o rules below can order-only-depend on it directly --
# under parallel make (-j), a *.PHONY sibling prerequisite of `pgo-build`
# (alongside $(PGO_BIN)) would NOT guarantee pgo-merge finishes before the
# use-pass .o's start compiling, since make is free to run independent
# prerequisites of the same target concurrently. Depending on the file
# itself gives a real edge in the dependency graph instead.
pgo-merge: $(PGO_PROFDATA)

$(PGO_PROFDATA): pgo-train
	@mkdir -p $(dir $@)
	$(LLVM_PROFDATA) merge -o $@ $(PGO_RAW_DIR)/*.profraw

# Step 4: rebuild with the merged profile.
pgo-build: $(PGO_BIN)

$(PGO_BIN): $(PGO_USE_OBJS)
	$(CC) $(LDFLAGS) $(PGO_USE_FLAG) -o $@ $(PGO_USE_OBJS)

$(PGO_USE_OBJDIR)/src/asm/%.o: src/asm/%.S
	@mkdir -p $(dir $@)
	$(FRONTEND_CC) $(CFLAGS) -c -o $@ $<

$(PGO_USE_OBJDIR)/src/c/%.o: src/c/%.c
	@mkdir -p $(dir $@)
	$(FRONTEND_CC) $(CFLAGS) -c -o $@ $<

# $(PGO_PROFDATA) is a NORMAL prerequisite here (not order-only): these
# .o's are the ones clang bakes the profile into, so when a re-run of
# `make pgo` regenerates merged.profdata (pgo-train is .PHONY, so it --
# and therefore $(PGO_PROFDATA) -- reruns every invocation), every
# clang-compiled object must be recompiled against the fresh profile,
# not just whichever .c files happened to change.
$(PGO_USE_OBJDIR)/%.o: %.c $(PGO_PROFDATA)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(PGO_USE_FLAG) -c -o $@ $<

$(PGO_USE_OBJDIR)/%.o: %.S $(PGO_PROFDATA)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(PGO_USE_FLAG) -c -o $@ $<

pgo-train-clean:
	rm -rf $(PGO_RAW_DIR)

pgo-clean:
	rm -rf $(PGO_DIR) $(PGO_GEN_BIN) $(PGO_BIN)

# --- -march=native (opt-in, for ll's own clang-compiled code only) -----
#
# Measured: a real, modest wall-clock win on pkgs.ffmpeg-full.outPath --
# but this machine is shared, and plain, unpinned `hyperfine` runs on it
# were noisy enough to flip sign run to run (one pair of early runs
# showed a misleadingly large ~1.09x-1.11x; later unpinned runs under
# heavier load showed ~1.02x the WRONG way). Pinning both sides to the
# same core (`taskset -c N`) to remove scheduling noise gave a
# consistent, repeatable ~1.04x speedup in both A/B orderings -- that's
# the number to trust, not the first noisy one. No correctness
# regression (44/44 difftest, byte-identical ffmpeg-full output vs
# asnix). Scoped the same way thin-LTO was tried (and reverted, see
# below): only the clang-compiled files under the generic `%.o: %.c`/
# `%.o: %.S` rules (src/llvm/*.c, src/llvm/ops/*.c, src/llvm/ll_main.c),
# NOT the FRONTEND_CC(gcc)-compiled vendored frontend.
#
# Why this is its OWN opt-in target (`make ll-native`) rather than a
# default-on flag on `ll` itself: -march=native bakes in the BUILD
# machine's exact CPU feature set -- the resulting binary can SIGILL on
# any other CPU. That's a correctness-adjacent tradeoff (not just a
# staleness one like PGO's), so it must never be silently the default.
# Builds into its own object directory/binary name, same reasoning as
# PGO above: a plain CFLAGS override on the existing `ll`/libll.a object
# paths would silently mix native and non-native .o's across successive
# `make ll` / `make ll-native` runs without a `make clean` in between,
# since make only tracks file mtimes, not which flags produced them.
#
# IMPORTANT, nix-specific gotcha: nixpkgs' own clang wrapper treats
# `-march=native`/`-mtune=native` as an "impure flag" and SILENTLY DROPS
# IT (just a `warning: Skipping impure flag ... because
# NIX_ENFORCE_NO_NATIVE is set`, not an error) unless NIX_ENFORCE_NO_NATIVE
# is unset/empty in the environment. Forgetting this makes `make
# ll-native` silently produce the exact same binary as `make ll` --
# verify with e.g. `objdump -d ll-native | grep vpermb` (an AVX512-VBMI
# instruction absent from the generic -O3 build) if in doubt. Usage:
#   NIX_ENFORCE_NO_NATIVE= make ll-native
NATIVE_DIR    := .native
NATIVE_OBJDIR := $(NATIVE_DIR)/obj
NATIVE_BIN    := ll-native
NATIVE_FLAG   := -march=native

NATIVE_ASM_OBJS  := $(addprefix $(NATIVE_OBJDIR)/,$(ASM_SRCS:.S=.o))
NATIVE_C_OBJS    := $(addprefix $(NATIVE_OBJDIR)/,$(C_LIB_SRCS:.c=.o))
NATIVE_MAIN_OBJ  := $(NATIVE_OBJDIR)/src/llvm/ll_main.o
NATIVE_OBJS      := $(NATIVE_ASM_OBJS) $(NATIVE_C_OBJS) $(NATIVE_MAIN_OBJ)
NATIVE_DEPS      := $(NATIVE_OBJS:.o=.d)

.PHONY: native-clean

$(NATIVE_BIN): $(NATIVE_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(NATIVE_OBJS)

$(NATIVE_OBJDIR)/src/asm/%.o: src/asm/%.S
	@mkdir -p $(dir $@)
	$(FRONTEND_CC) $(CFLAGS) -c -o $@ $<

$(NATIVE_OBJDIR)/src/c/%.o: src/c/%.c
	@mkdir -p $(dir $@)
	$(FRONTEND_CC) $(CFLAGS) -c -o $@ $<

$(NATIVE_OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(NATIVE_FLAG) -c -o $@ $<

$(NATIVE_OBJDIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(NATIVE_FLAG) -c -o $@ $<

native-clean:
	rm -rf $(NATIVE_DIR) $(NATIVE_BIN)

# --- Full LTO (make ll-flto): opt-in, NOT part of `make ll`/`all` -------
#
# `-flto=thin` (the parallelized, summary-based cross-TU analysis) was
# tried first and found nothing: passed both correctness gates but gave
# no measurable win on pkgs.ffmpeg-full.outPath, if anything a ~1.03x
# slowdown within hyperfine's noise on this shared machine. `-flto=full`
# (whole-program analysis, not just per-TU summaries) found a real one:
# 0.41% fewer instructions on the same held-out expression (callgrind,
# deterministic, reproduced on rerun), ~1.01x wall-clock -- directionally
# consistent with the Ir count but not independently conclusive on its
# own given hyperfine's noise floor on this machine. Full LTO's usual
# slow-monolithic-link downside doesn't bite at this codebase's size
# (link time here is a fraction of a second).
#
# Same scope as -march=native above: only the clang-compiled files
# (src/llvm/*.c, src/llvm/ops/*.c, src/llvm/ll_main.c) get -flto=full;
# the FRONTEND_CC(gcc)-compiled vendored frontend (src/c/*.c, src/asm/*.S)
# is compiled identically to `make ll`, same reasoning as PGO/native
# above (gcc doesn't emit LLVM bitcode, so there's nothing to LTO there
# even if it understood the flag). The LINK step mixes LLVM-bitcode .o's
# (from the clang+flto compiles) with plain ELF .o's (from the gcc
# compiles) -- clang's own flto driver logic handles that mix correctly,
# but the link itself needs `-fuse-ld=lld`: neither gold nor bfd (the
# binutils linker already in this devShell) carries the LLVM bitcode LTO
# plugin lld has built in. `nix develop`'s own `llvmPackages_19.lld`
# (flake.nix) is what provides it.
FLTO_DIR    := .flto
FLTO_OBJDIR := $(FLTO_DIR)/obj
FLTO_BIN    := ll-flto
FLTO_FLAG   := -flto=full

FLTO_ASM_OBJS := $(addprefix $(FLTO_OBJDIR)/,$(ASM_SRCS:.S=.o))
FLTO_C_OBJS   := $(addprefix $(FLTO_OBJDIR)/,$(C_LIB_SRCS:.c=.o))
FLTO_MAIN_OBJ := $(FLTO_OBJDIR)/src/llvm/ll_main.o
FLTO_OBJS     := $(FLTO_ASM_OBJS) $(FLTO_C_OBJS) $(FLTO_MAIN_OBJ)
FLTO_DEPS     := $(FLTO_OBJS:.o=.d)

.PHONY: flto-clean

$(FLTO_BIN): $(FLTO_OBJS)
	$(CC) $(LDFLAGS) -fuse-ld=lld $(FLTO_FLAG) -o $@ $(FLTO_OBJS)

$(FLTO_OBJDIR)/src/asm/%.o: src/asm/%.S
	@mkdir -p $(dir $@)
	$(FRONTEND_CC) $(CFLAGS) -c -o $@ $<

$(FLTO_OBJDIR)/src/c/%.o: src/c/%.c
	@mkdir -p $(dir $@)
	$(FRONTEND_CC) $(CFLAGS) -c -o $@ $<

$(FLTO_OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(FLTO_FLAG) -c -o $@ $<

$(FLTO_OBJDIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(FLTO_FLAG) -c -o $@ $<

flto-clean:
	rm -rf $(FLTO_DIR) $(FLTO_BIN)

-include $(NATIVE_DEPS)
-include $(PGO_GEN_DEPS)
-include $(PGO_USE_DEPS)
-include $(FLTO_DEPS)
-include $(DEPS)
-include src/llvm/ll_main.d
