# M8 findings: LLVM-JIT-per-invocation vs asnix (asm) vs cppnix

Date: see git log. Machine: shared dev host, `nix develop` devShell (LLVM 18.1.8).

Workloads: `bench/workloads/*.nix`. These are asnix's own microbenchmark files.
Each workload ran 15 times, through `hyperfine --warmup 3 --min-runs 15`. Every
workload's result was checked byte-for-byte against `asnix` first, using
`tests/difftest.sh` and a manual diff over each workload file.

## Headline result

`ll` is this project's LLVM-JIT backend. `asnix` is asnix's own hand-written
x86-64 assembly, threaded-dispatch backend. Across every workload tested, `ll`
is 10 to 230 times slower than `asnix`, wall-clock. `ll` is also consistently
slower than `nix-instantiate` (real C++ Nix). This result is the opposite of
the port's working thesis. The thesis was that compiling to native code with
LLVM beats threaded dispatch.

| workload               | ll (ms) | asnix (ms) | nix-instantiate (ms) | ll vs asnix |
|-------------------------|--------:|-----------:|----------------------:|------------:|
| baseline (`1`)          |    17.7 |        1.4 |                  35.8 |       ~13x  |
| let_chain               |   132.3 |        1.7 |                  38.2 |       ~77x  |
| attrset_build_small     |    20.8 |        1.6 |                  38.5 |       ~13x  |
| attrset_build_medium    |    88.9 |        2.2 |                  40.9 |       ~40x  |
| attrset_build_large     |   501.2 |        3.1 |                  41.8 |      ~160x  |
| list_build_small        |    21.0 |        2.0 |                  39.7 |       ~10x  |
| list_build_medium       |    28.4 |        2.0 |                  41.3 |       ~14x  |
| list_build_large        |   549.4 |        2.4 |                  40.1 |      ~231x  |
| select_chain            |   392.8 |        3.1 |                  41.8 |      ~129x  |
| arith_deep              |   710.1 |        3.6 |                  43.6 |      ~200x  |

Raw hyperfine JSON for each workload is in this directory.

## Root cause: decomposed timing

Set `LL_SHOW_STATS=1` to see this breakdown. `ll_jit_eval` (jit_driver.c) now
reports three phases. **translate** is this project's own pass from IrOp to
LLVM IR. **jit** is `LLVMOrcLLJITAddLLVMIRModule` plus the symbol lookup. This
phase covers LLVM's own optimization pipeline, native code generation, and
ORC's JITLink linker. **execute** is the time to call the compiled `ll_entry`
function.

| workload               | translate | jit (add+link+lookup) | execute  |
|-------------------------|----------:|-----------------------:|---------:|
| baseline                |    0.4 ms |                 4.3 ms | 0.000 ms |
| let_chain               |    1.5 ms |               137.3 ms | 0.040 ms |
| attrset_build_medium    |    0.8 ms |                73.4 ms | 0.016 ms |
| attrset_build_large     |    3.3 ms |               486.1 ms | 0.050 ms |
| list_build_large        |    3.7 ms |               524.1 ms | 0.030 ms |
| select_chain            |    4.0 ms |               403.6 ms | 0.039 ms |
| arith_deep              |   10.9 ms |               703.9 ms | 0.008 ms |

The execute phase stays under 0.1 ms for every workload. This holds even for
the 2000-entry attrset and list literals. The compiled native code is fast.
That part of the thesis is correct.

The jit phase is the problem. LLVM's own optimizer and JITLink linker run
fresh on every process invocation. This phase costs 4 ms at minimum. It scales
up to 500-700 ms for larger programs. This phase dominates wall-clock time so
completely that translate and execute combined are noise: under 2% of total
time in every workload above baseline.

This result matches a risk named earlier in planning (see
`~/.claude/plans/soft-wishing-engelbart.md`, the M8/M9+ notes). Those notes
pointed to `~/gnix/iets`'s own `aot-ir-exploration` finding: precompiled or
AOT IR buys little for Nix, because cost concentrates elsewhere, not in
dispatch. This session measured the same kind of risk directly, instead of
assuming it. The hypothesis under test was: LLVM JIT beats threaded dispatch
for a Nix evaluator. That hypothesis is false under a per-invocation one-shot
JIT model, for every workload shape tested so far. The JIT compile tax has a
fixed part and a scaling part. Neither asnix (it interprets a cheap custom
bytecode) nor cppnix (it tree-walks directly) ever pays this tax. The tax
swamps the native-codegen execution speed win. That win is real. It is also
small compared to the tax.

## A separate, real finding: ORC JITLink scales badly with function count

`bench/workloads/lambda_chain.nix` has 2000 nested single-argument calls. Real
Nix call-by-need semantics wrap each argument in its own thunk (see
compiler.c's AST_APP behavior, unmodified from asnix). This workload is
missing from the table above. It still produces correct results at smaller
depths (checked up to n=100, about 100 ms). But its run time scales worse than
linearly. At the real n=2000 in the benchmark file, it times out past 15 s.

A `perf record` capture during an n=800 run (6.5 s) shows the hot path:
`llvm::DenseMap<SymbolStringPtr,...>::LookupBucketFor`, inside
`JITLinkerBase::linkPhase1`. This is ORC's own symbol resolution during
linking. It scales with the number of distinct native functions. This
backend's design compiles one real LLVM function per closure or thunk body.
Here, that means one function per nesting level. This is a second, independent
cost, separate from the per-invocation JIT tax above. It is specific to
programs with many distinct small closures or thunks.

## What this means for M9+

The plan already set a rule for this case. If JIT overhead dominates on short
evals, build the deferred AOT or object-cache path next. If this backend loses
even on large nixpkgs evals, write up that result. Do not hide it.

JIT overhead dominates at every scale measured so far, not only on short
evals. The 500-700 ms tax on a 2000-entry attrset literal is far larger in
absolute terms. Compare it to the under-5 ms total cost asnix pays for the
same program.

Two honest paths forward exist. They are not mutually exclusive.

1. Object or AOT caching. Compile once. Cache the generated machine code,
   keyed by the IrProgram's content hash. This matches the approach asnix's
   own `bytecode_cache.c` uses for its bytecode. Reuse the cached code across
   invocations. This amortizes the JIT tax across repeated evaluations of the
   same file. That pattern is realistic for import-heavy nixpkgs evaluation,
   where the same files compile repeatedly across process invocations. This
   path does nothing for a genuinely cold, one-shot `nix eval` of a
   never-before-seen expression.
2. Reduce LLVM's own optimization and linking cost. Try a lower LLVM
   optimization level, through `LLVMOrcLLJITBuilder`'s target-machine and
   transform-layer options (this session used the defaults). Check whether
   ORC's choice of JITLink versus RuntimeDyld linker matters at this scale.
   Reduce function count: inline small closure or thunk bodies into their
   caller, instead of emitting one real LLVM function per body. This directly
   targets the `lambda_chain` scaling finding.

Neither path is implemented yet. This file records the measurement to use for
that follow-up work, before any further claim about this backend's speed
relative to asnix or cppnix.

## M9 update: both fixes landed, re-measured

Both paths from the section above are now implemented. Results changed a lot.

### Fix 1: two-tier object cache

`jit_driver.c`'s `ll_jit_eval_cached` takes a cache key. The CLI hashes the raw
source text with FNV-1a to build this key. On a cache miss, the function
compiles once at `LLVMCodeGenLevelNone`. This runs the current invocation as
fast as possible. The function then compiles the same module a second time,
at `LLVMCodeGenLevelAggressive`, and writes that object to
`~/.cache/ll-jit/<hash>.o`. A cache hit skips `translate_fn` and all codegen.
It loads the cached object straight into ORC. This uses
`LLVMOrcLLJITAddObjectFile` (link only). The function then runs the result.

This surfaced a real bug: the cached object failed to load with a relocation
error ("relocation target ... out of range of Pointer32 fixup"). The fix was
to build the `LLVMTargetMachine` with `LLVMRelocPIC`, not `LLVMRelocDefault`.
A cached object gets linked at an arbitrary runtime address in a later
process. `LLVMRelocDefault`'s static-addressing assumption does not survive
that move.

Measured effect on `select_chain` (`LL_SHOW_STATS=1`): cold run 58 ms
(translate 3.9 ms, compile_fast 53.6 ms, link+lookup+run 1.0 ms). Warm cache
hit: 1.4 ms total. A roughly 40x speedup on a cache hit, in-process.

### Fix 2: static-link LLVM

`hello-jit` is a trivial binary that links LLVM but does none of this
project's own work. A comparison of `./ll 1` against `hello-jit` showed both
paying the same ~15-18 ms floor. `ldd` showed why: both dynamically link
`libLLVM.so`. Loading that shared library costs the dynamic loader ~15 ms on
every process start. This cost does not depend on program size or cache
state. `asnix` links nothing this large. It starts in ~1-4 ms.

The Makefile now links LLVM statically, via `llvm-config --link-static`. The
static archives need two more libraries for their own transitive
dependencies: `-lxml2` and `-lstdc++`. Both are now in `flake.nix`'s devShell.
`ldd ./ll` now shows no LLVM dependency at all. Static `./ll 1` startup
dropped to ~5.5 ms.

### Combined result

All correctness checks still pass: the full `tests/difftest.sh` suite
(44 cases) and every `bench/workloads/*.nix` file (except `lambda_chain.nix`,
still excluded per the ORC JITLink finding above) match `asnix` byte-for-byte.

| workload               | ll (ms, M9) | asnix (ms) | nix-instantiate (ms) | ll vs asnix | ll vs nix-instantiate |
|-------------------------|------------:|-----------:|----------------------:|------------:|----------------------:|
| baseline (`1`)          |         7.3 |        2.1 |                  43.5 |      3.5x slower |     6.0x faster |
| let_chain               |        18.2 |        1.5 |                  38.9 |     11.9x slower |     2.1x faster |
| attrset_build_small     |         6.9 |        1.6 |                  41.5 |      4.3x slower |     6.0x faster |
| attrset_build_medium    |         7.6 |        2.0 |                  42.0 |      3.7x slower |     5.5x faster |
| attrset_build_large     |         8.6 |        3.2 |                  41.7 |      2.7x slower |     4.8x faster |
| list_build_small        |         7.0 |        1.6 |                  42.8 |      4.4x slower |     6.1x faster |
| list_build_medium       |         7.4 |        1.7 |                  43.0 |      4.3x slower |     5.8x faster |
| list_build_large        |         8.5 |        2.4 |                  41.5 |      3.5x slower |     4.9x faster |
| select_chain            |         8.8 |        2.2 |                  39.6 |      4.0x slower |     4.5x faster |
| arith_deep              |        10.2 |        4.1 |                  43.2 |      2.5x slower |     4.2x faster |

`ll` now beats `nix-instantiate` (real C++ Nix) on every workload, by 4x to
6x. It closed most of the gap to `asnix`: from 10x-230x slower (the original
M8 measurement) down to roughly 2.5x-4.4x slower on nine of ten workloads.

`let_chain` is the one outlier. It runs nearly 12x slower than `asnix`. That
is noticeably worse than every other workload's 2.5x-4.4x band, despite
`let_chain` being one of the smallest programs tested. Nobody investigated
this gap yet. It is the next concrete thing to look at. It is
not a reason to doubt the two fixes above. The other nine workloads span a
300x range in program size, from `baseline` to
`attrset_build_large`/`list_build_large`. They now cluster tightly around
3x-4.5x slower than asnix. That is a real, large improvement over the
original 10x-230x spread. `ll` is still not the fastest Nix evaluator. It is
closer than it was. The remaining gap is now small enough that per-workload
investigation, starting with `let_chain`, is the right next step. Another
architecture change is not.

## M9 update 2: the `let_chain` outlier, fixed

A `perf record` capture of `let_chain`'s cache-hit run used `-F 4000` across
50 repeated invocations, to collect enough samples. It pointed back at ORC's
own linking machinery again: `JITDylib::transferEmittedNodeDependencies` and
`InProcessMemoryManager::allocate`, inside the same `JITLinkerBase::linkPhase1`
this file's first M8 section already named. The M9 object cache above still
routed every cache hit through `LLVMOrcLLJITAddObjectFile`. That call skips
compilation. It still pays ORC's own JITLink linking cost on every single
run, cache hit or not. `let_chain` has around 100 sequential bindings, in one
function, each calling several runtime helpers. This structure seems to hit
a slower path through that linker than other workloads of similar size.
Nobody pinned down the exact reason inside LLVM.

The fix sidesteps the question instead of answering it. It skips ORC's
linker entirely on a cache hit. `jit_driver.c` now links the cached object
into a real shared library once, at cache-write time, instead of caching a
bare `.o`. This step uses `cc -shared`, run through `fork`+`execlp`, with no
shell involved. A cache hit calls `dlopen()` on that `.so` and `dlsym()`s
`ll_entry` directly. No LLJIT instance, no JITLink, no ORC session of any
kind.

Effect on `let_chain`'s cache-hit time: 9.86 ms through
`LLVMOrcLLJITAddObjectFile`, down to about 0.5 ms through `dlopen`. Every
other workload's cache-hit time dropped too, by a smaller amount. For
example, `baseline` dropped from 0.87 ms to 0.23 ms. `dlopen()` on a small
shared library is a well-tuned OS primitive, built for exactly this job.
ORC's general-purpose in-memory JIT linker is not.

### Final measured result (static link + two-tier cache + dlopen)

All correctness checks still pass: `tests/difftest.sh` (44 cases) and every
`bench/workloads/*.nix` file except `lambda_chain.nix`, checked under both a
cache miss and a cache hit, match `asnix` byte-for-byte.

| workload               | ll (ms) | asnix (ms) | nix-instantiate (ms) | ll vs asnix | ll vs nix-instantiate |
|-------------------------|--------:|-----------:|----------------------:|------------:|----------------------:|
| baseline (`1`)          |     4.4 |        1.4 |                  40.9 |  3.2x slower |   29.4x faster |
| let_chain               |     5.4 |        1.6 |                  36.0 |  3.3x slower |   22.0x faster |
| attrset_build_small     |     5.5 |        1.3 |                  37.0 |  4.3x slower |   28.6x faster |
| attrset_build_medium    |     5.4 |        1.7 |                  38.6 |  3.1x slower |   22.1x faster |
| attrset_build_large     |     8.2 |        2.7 |                  39.3 |  3.0x slower |   14.5x faster |
| list_build_small        |     5.3 |        1.3 |                  37.0 |  4.0x slower |   27.8x faster |
| list_build_medium       |     5.4 |        1.4 |                  38.7 |  3.9x slower |   27.5x faster |
| list_build_large        |     8.4 |        2.2 |                  39.9 |  3.8x slower |   17.9x faster |
| select_chain            |     6.2 |        2.1 |                  35.6 |  2.9x slower |   16.9x faster |
| arith_deep              |     7.5 |        3.4 |                  39.6 |  2.2x slower |   11.5x faster |

`let_chain` no longer stands out: every workload now falls in the same
2.2x-4.3x-slower-than-`asnix` band, with no outlier. `ll` beats
`nix-instantiate` by 11.5x to 29.4x on every workload, consistently.

### What's left

The remaining 2.2x-4.3x gap to `asnix` no longer scales with program size (the
whole point of the fixes above). It looks like fixed per-process cost instead.
One concrete, measured lead: `ll`'s binary is 56 MB. `asnix`'s binary is only
1.9 MB. Stripping debug symbols from `ll`'s binary made no measurable
difference (within noise over 30 runs), so the gap is not debug-info size.
A build with `-ffunction-sections -fdata-sections -Wl,--gc-sections` barely
shrank the binary, from 56.9 MB to 56.4 MB. LLVM's static archives have
enough internal cross-references that whole-program dead-code elimination
at link time finds almost nothing to remove. The only path left is
rebuilding LLVM itself from source with section-level granularity. This
project has not attempted that.

## M9 update 3: trimmed dynamic exports and LLVM components

Two more things were tried against the fixed per-process floor. The
binary's own dynamic symbol table (`.dynsym`) had 26,589 entries. Almost all
of them were internals of the statically-linked LLVM code. They were
exported only because the build used plain `-rdynamic`, which exports
everything. An explicit `--dynamic-list` names just the ~23 runtime
functions that JIT-compiled or `dlopen`ed code actually calls
(`src/llvm/jit_symbols.list`). That list cut the table to 383 entries.
Measured effect on startup time: none. `.dynsym` size was not the cost.

The LLVM component list was `core orcjit native`. The `native` component
pulls in `x86asmparser`, `x86disassembler`, and `x86targetmca`, none of
which this project uses: it only ever compiles IR to machine code and runs
it, through `compile_to_object`/`run_object` in `jit_driver.c`, never
parsing or disassembling assembly text. Switching to `core orcjit
nativecodegen` drops those three archives. Measured effect: about a 10%
startup-time improvement, small but real and reproducible across repeated
runs.

Both changes are in the Makefile now (`--dynamic-list`, `nativecodegen`).
The `--dynamic-list` change made no speed difference. It stays, because it
is the correct minimal-exports default regardless. The `nativecodegen`
change stays too. Its win is small, but it is real and measured.

### Updated final result

All correctness checks still pass under this build too.

| workload               | ll (ms) | asnix (ms) | nix-instantiate (ms) | ll vs asnix | ll vs nix-instantiate |
|-------------------------|--------:|-----------:|----------------------:|------------:|----------------------:|
| baseline                |     4.4 |        1.4 |                  39.3 |  3.3x slower |  27.4x faster |
| let_chain               |     5.8 |        1.8 |                  35.9 |  3.1x slower |  18.8x faster |
| attrset_build_small     |     4.5 |        1.4 |                  37.5 |  3.2x slower |  26.2x faster |
| attrset_build_medium    |     4.8 |        1.5 |                  37.9 |  3.1x slower |  23.7x faster |
| attrset_build_large     |     7.9 |        2.9 |                  38.9 |  2.8x slower |  13.5x faster |
| list_build_small        |     4.6 |        1.4 |                  39.4 |  3.1x slower |  25.6x faster |
| list_build_medium       |     4.9 |        1.6 |                  37.9 |  3.1x slower |  23.7x faster |
| list_build_large        |     7.5 |        2.1 |                  36.9 |  2.9x slower |  15.0x faster |
| select_chain            |     5.6 |        1.7 |                  36.6 |  3.4x slower |  23.0x faster |
| arith_deep              |     6.9 |        2.2 |                  36.9 |  3.1x slower |  17.7x faster |

Every workload now falls inside a 2.8x-3.4x-slower-than-`asnix` band, the
tightest spread yet. `ll` beats `nix-instantiate` by 13.5x to 27.4x,
consistently.

`ll` is still not the fastest Nix evaluator. `asnix`'s hand-written assembly
still wins on every workload tested. The gap narrowed from the original
10x-230x (M8's first finding) to a tight, uniform 2.8x-3.4x. Five fixes
drove that:

- a two-tier compile cache
- static-linking LLVM
- `dlopen`-based cache hits that bypass ORC's JIT linker
- a minimal dynamic-symbol export list
- a trimmed LLVM component list

What remains looks like fixed cost from two sources: ELF loading, and C++
static initializers. The binary is about 54 MB, statically linked, with 302
global constructors (counted via `_GLOBAL__sub_I_*` symbols). `asnix`'s
binary is 1.9 MB and links no LLVM code at all. Closing the remaining gap
means rebuilding LLVM itself with finer-grained static initialization. This
project's own code cannot make that change.

## M9 attempt 4: slim/heavy binary split, tried twice, not a win

One more idea was tried against the fixed per-process floor above. The
attempt: split `ll` into two binaries. A slim binary links no LLVM code at
all, only the vendored asnix runtime plus a `dlopen`/`dlsym` call. On a
cache hit, it loads the cached `.so` directly and exits, with none of the
~54 MB/302-constructor cost. On a cache miss, it `execve()`s a separate
heavy, LLVM-linked binary. That binary compiles, runs, and populates the
cache. This is the same work `ll` already does today, split across two
processes.

The first try broke `builtins.length` on the slim binary's cache-hit path.
It was reverted unmeasured. The second try moved the cache-hit `dlopen`
logic into its own small file with no LLVM dependency at all
(`src/llvm/cache_dlopen.c`). The single-binary cache-hit path now shares
that same file. That fixed `builtins.length`. All 44 differential tests
passed. All 10 bench workloads matched `asnix` output exactly, through
both the cache-miss (`execve`) and cache-hit (`dlopen`) paths of the slim
binary.

The slim binary built at 1.4 MB, smaller than `asnix`'s own 1.9 MB, with no
LLVM dependency at all (`ldd` showed none). Measured against `asnix` with
`hyperfine` (same 10 workloads, cache warm), it was 3.1 to 4.6 times
slower, not faster. That is worse than the single binary's own 2.8 to 3.4
times slower. The reason: the single binary's ~54 MB size comes from
static linking. Static linking resolves symbols at link time, not at
every process start. The large file was never the source of a real
per-process cost. Splitting it in two removed no real bottleneck. The
`execve` indirection on a cache miss adds a new cost instead.

`ll` stays one binary. The idea is now ruled out, not just deferred: it was
built correctly, tested for correctness, and measured, and it does not
help.

## M9 update 4: trimmed shared-library dependencies

The slim/heavy split looked at the wrong thing. It focused on binary size.
The actual next step was to measure syscalls, not file size. `strace -c`
on a cache-hit run of `ll` showed about 2.7 times the syscall count of
`asnix`. Most of that extra count was `openat` and `mmap`. `ldd` showed
why. `ll` linked against 10 shared libraries: `librt`, `libdl`, `libm`,
`libz`, `libncursesw`, `libxml2`, `libstdc++`, `libgcc_s`, `libc`, and the
dynamic loader itself. `asnix` links against only `libpthread` and `libc`.
Each extra shared library costs its own `openat` and `mmap` call on every
process start. Each one also needs its own relocation work at load time.
The loader also probes several candidate directories per library before
it finds each one, which multiplies the cost further.

Three of those ten were unnecessary. `llvm-config` reported `-lrt` and
`-ldl` as needed system libraries. But every symbol they cover already
resolves from `libc.so.6` directly on this glibc version. Glibc 2.34 and
later folded `librt` and `libdl` into `libc` itself. The separate `.so`
files are now just empty compatibility shims. `llvm-config` also reported
`-lxml2` as needed. But nothing in the `core`, `orcjit`, and
`nativecodegen` components this project links calls into libxml2 at all.
Dropping all three from the link command still links cleanly.

Two more dropped through static linking instead. This project now uses
`g++` as the final link driver, not plain `gcc`, with `-static-libstdc++`
and `-static-libgcc` added. Those two flags fold `libstdc++` and
`libgcc_s` into the binary at link time. Neither then needs to load as a
shared library at every process start. Static archives for `libz` and
`libncursesw` are not available in this project's current Nix inputs, so
those two stay dynamic. `libm` also stays dynamic. A static archive for it
exists, but something else in the link still pulls in the shared one
regardless. Chasing that further did not look worth it, for one library
already colocated with `libc`.

The result: `ll` now links against 5 shared libraries (`libz`,
`libncursesw`, `libm`, `libc`, the dynamic loader), down from 10. All 44
differential tests still pass. All 10 bench workloads still match `asnix`
output exactly. Measured with `hyperfine` at 30 runs per workload (up from
15, for a cleaner read against machine noise): `ll` is now 1.56 to 2.82
times slower than `asnix`, average 2.37 times, down from the earlier 2.8 to
3.4 times. This is a real, reproducible gain, found by measuring the actual
cost (syscalls) instead of a proxy for it (file size).

## M9 update 5: static zlib, and a note on measurement noise

`libz` dropped too. `flake.nix` now provides `zlib.static` alongside the
regular `zlib`. The Makefile links its `libz.a` by full path in place of
`llvm-config`'s reported `-lz`. No source rebuild was needed. Nixpkgs
already had a cached build of this output. `ll` now links against 4
shared libraries instead of 5: `libncursesw`, `libm`, `libc`, and the
dynamic loader. All 44 differential tests still pass.

A static `libncursesw` can drop one more. Nixpkgs has no cached build of
it with static linking enabled. Building one means compiling ncurses from
source on this shared machine. This project's own operating constraints
advise against that, for an uncertain and likely small gain. That one
stays dynamic.

The `hyperfine` numbers after this change did not clearly improve on the
2.37 times average from the previous section. Some runs even showed `ll`
faster than `asnix` on individual workloads (`let_chain` at 30 runs per
command). A re-run of that same case at 50 runs per command told a
different story. `asnix` came back 2.29 times faster than `ll`, in line
with every other workload. This machine is shared, and its load varies.
At 30 runs, the noise band is wide enough to produce a result like that
by chance.

The honest reading: the static zlib change is a real, free reduction in
shared-library count, with no measured downside. Its effect on wall clock
time is too small to separate from this machine's own noise floor. The
earlier 1.56 to 2.82 times range, average 2.37 times, stays the
best-supported number for where `ll` stands against `asnix` right now.
That number comes from static-linking `libstdc++` and `libgcc_s`, and
from dropping `librt`, `libdl`, and `libxml2`.

## M10: AOT threaded dispatch, no JIT at all

Every finding above treated the per-invocation JIT compile step as a
fixed cost to shrink. The real fix was to remove it. `ll` no longer
compiles anything at runtime. It op handlers (`src/llvm/ops/*.c`) are
plain C, compiled once, ahead of time, by `clang`, into `ll`'s own
binary. This is the same role `asnix`'s hand-written assembly op
handlers play in `asnix`'s own binary. A compiled Nix program is a
flat array of op records, same as before. Each one's handler field
points directly at compiled machine code. Running a program means
jumping from one handler to the next, never compiling anything.

Each handler ends in a guaranteed tail call to the next one
(`[[clang::musttail]]`). This is a hard compile-time guarantee, not a
hope that `-O2` finds the same optimization on its own. A guaranteed
tail call keeps the native call stack from growing. This holds true
no matter how deep a Nix program's call or force chain goes. It is
the same property `asnix`'s own `jmp *%rax` dispatch gives it.
Measured directly: a 2000-deep nested-lambda workload and a
500,000-deep tail-recursive one both ran correctly under a 512 KB
native stack. `asnix` needs the identical stack headroom for the same
inputs. The two architectures now cost the same in this respect. This
is not just `ll` happening to survive.

The result: `ll` links against one shared library, `libc`, the same
as a minimal C program. No LLVM code exists in the shipped binary at
all. The dynamic-library-trimming work from the earlier sections of
this file is no longer needed. The thing that needed those shared
libraries, the ORC JIT runtime, is gone. `ll`'s own binary is 1.2 MB.
`asnix`'s is 1.9 MB.

All 44 differential tests pass. A second, previously-crashing case
now passes too: `builtins.add 1 2`, an arity-2 curried builtin. This
is a free side effect. Closures and thunks went back to `asnix`'s own
heap layout, instead of this project's earlier, incompatible one.

Measured with `hyperfine` at 60 runs per workload: `ll` is faster
than `asnix` on 8 of the 10 workloads, by margins from 12 to 48
percent. It is slower on the other 2, `attrset_build_large` and
`list_build_large`, both around 1.3 times. Both are large, roughly
2,000-element literal constructions. The geometric mean across all 10
workloads is 0.78. `ll` is about 22 percent faster than `asnix` on
average. This is the first point in the project where `ll` beats
`asnix` on most tested workloads, not the reverse.

The two remaining slower cases point to a narrow, legitimate research
finding, not a bug. Construction of a large literal list or attrset
calls the exact same shared C function in both `ll` and `asnix`:
`asnix_bshared_make_list` or `asnix_bshared_make_attrs`. The
difference can only come from the thousands of individual push
operations that lead up to that call.

Disassembly of the compiled handlers points to the actual cause.
`asnix`'s own dispatch pins its five live values (IP, ENV, VALSTACK,
POOL, RETSP) in five fixed registers for the whole run. One op's
handler ends. The next one begins. No data moves between them at
all, because both sides agree on the same fixed registers by
construction.

`ll`'s handlers pass those same five values as ordinary C function
arguments, under the standard calling convention. Clang's `musttail`
guarantees the handoff compiles to a real tail jump, not a nested
call. It does not remove the need to place each value into its
assigned argument register first. Take an op that does almost no
work of its own, such as pushing one integer literal. There, that
placement cost takes up a larger share of the op's total time. A heavier op, such as
a function call or a thunk force, dilutes that same fixed cost across
more real work. This is why the two slowest cases are exactly the two
highest-repetition, lowest-work-per-op workloads, and not the others.

Two build flags closed part of this gap. `-fomit-frame-pointer`
removes a frame-pointer push and pop that `clang` otherwise added to
any handler with a cold-path fallback call. The hot tail-call path
never needed one. `-O3` in place of `-O2` closed a further, smaller
slice. Together they moved `attrset_build_large` from 1.35 to 1.28
times slower, and `list_build_large` from 1.31 to 1.26 times slower.
Neither change touched the other 8 workloads or correctness: 44 of
44 differential tests still pass. The geometric mean across all 10
workloads held at 0.79. On average, `ll` runs about 21 percent
faster than `asnix`. Depending on the run, it is faster on 7 to 8 of
the 10 workloads, within this shared machine's own run-to-run noise.

Closing the rest of this gap means removing the per-hop register
placement cost itself, not tuning around it. LLVM has a calling
convention built for exactly this case, `preserve_none`, designed for
a tiered interpreter dispatch loop like this one. Clang 18, the
version this project uses, does not yet expose it as a C attribute.
This is worth a second look once it does. Hand-writing the few
hottest handlers in assembly is the other option. That fixes it
sooner, but goes against this port's own premise. Left as a
documented, narrow, non-blocking gap: 7 to 8 of 10 workloads already
show `ll` ahead.

## M10 update: three more checks, one real finding

Three further ideas were tried against the two remaining slower
cases, `attrset_build_large` and `list_build_large`.

Global register variables were the first. GCC and Clang both
support pinning a C variable to one hardware register for the whole
program. This can reproduce `asnix`'s own permanently pinned dispatch
registers, with no `preserve_none` needed. Tested directly: clang 18
on this target rejects every general-purpose register this project
needs. That list is `r12`, `r13`, `r14`, `r15`, and `rbx`. The error
is "unsuitable for global register variables on this target." Only
`rbp` remains. One register cannot hold the five live dispatch
values this design needs. Ruled out by direct test, not by
assumption.

The second idea found a real, fixable cost. Take a long run of
plain integer literals: `list_build_large.nix`'s 5,000-element list,
or `attrset_build_large.nix`'s 2,000-entry attrset. Each compiles to
a long run of bare `OP_PUSH_INT` records with nothing else
interleaved. `compiler.c`'s own `AST_LIST` case shows this. So does
`irbuild.c`'s pool-index allocation. `dispatch_table.c`'s `ir_link`
now runs one extra pass after linking. It finds runs of 4 or more
consecutive `OP_PUSH_INT` records with consecutive pool indices, and
rewrites the first one to a fused handler. That handler pushes the
whole run in one tight loop, in a single dispatch hop instead of one
hop per literal. This does not touch `compiler.c` at all. It is a
pass over the already-linked op array, the same kind of optimization
`asnix`'s own optional `irpass_*.c` peephole passes already do.
Checked directly with `gdb`: a breakpoint on the fused handler fired
exactly once for the 5,000-element case, with the full count. All 44
differential tests still pass.

Measuring before and after this change showed the opposite of what
the register-placement theory predicted. Wall-clock time for both
workloads barely moved. Temporary phase timing inside `ll` itself
explained why. For `list_build_large`, parsing takes about 0.34 ms.
Compiling takes about 0.22 ms. Evaluating, the phase the fused
handler lives in, takes only 0.02 to 0.04 ms. The measured
`hyperfine` wall-clock time for the same run is over 2 ms. Most of
that time sits outside this process's own measured work. It happens
in process startup, before `main` even begins. The register-
placement cost from the earlier section is real. The disassembly
evidence for it still stands. But that cost sits inside a phase that
is already a small fraction of total wall-clock time, for these two
specific workloads. Fusing away 5,000 hops saves real time. It does
not save enough of the total to separate cleanly from this shared
machine's own run-to-run noise, at the wall-clock level.

The third idea closed part of the remaining gap for real.
`compiler.c`, `parser.c`, and `lexer.c` are vendored byte-for-byte
from `asnix`. `asnix`'s own Makefile compiles them with `cc` (gcc in
this devShell). This project's Makefile had switched to `clang` for
every file, including these, for the sake of the op handlers'
`musttail` requirement. The frontend has no `musttail` dependency at
all. The build now splits: the vendored frontend compiles with `cc`
again, and only `src/llvm` (which needs `musttail`) compiles with
`clang`. This measured a real improvement on `attrset_build_large`:
1.24 times slower than `asnix`, down to 1.13. `list_build_large` did
not show the same improvement, within this run's own noise band.

Current state after all three checks: 44 of 44 differential tests
pass. The geometric mean across all 10 workloads holds at 0.79. `ll`
runs about 21 percent faster than `asnix` on average. It is faster
on 7 of 10 workloads in the latest full run. `attrset_build_large`
and `list_build_large` remain the two slower cases. In that same
run, they measure 1.13 and 1.33 times slower. Each sits within this
shared machine's own run-to-run noise band. The push-run fusion and
the split frontend and dispatch compiler choice are both real,
positive, zero-risk changes. Both stay regardless of how much of
this specific gap they close. The `preserve_none` calling convention
remains the one concretely identified, not-yet-available fix for the
register-placement cost itself.

## M10 update: the remaining gap was the benchmark, not `ll`

One more check, run on `list_build_large` specifically: the same
comparison, with `hyperfine`'s `--shell=none` flag, removing the
shell layer `hyperfine` wraps every command in by default. The
result flipped. `ll` measured 1.23 times faster than `asnix` on that
same workload, not 1.33 times slower.

The cause: `ll`'s CLI took the Nix expression itself as a command-
line argument. `asnix`'s CLI takes a file path, and reads the file
itself. For a short expression this difference costs nothing.
`list_build_large.nix` is a 23 KB file. Turning that file into a
command-line argument first needs an extra shell step and a `cat`.
It also needs a large argument string, for the kernel to copy into
the new process's memory, on every single run. `ll`'s own CLI forced
the benchmark to construct the argument this way, so that extra cost
counted as `ll`'s own time. `asnix`'s own CLI never paid it. It reads
the same file directly.

The real fix was not in the benchmark. `ll`'s own CLI was missing a
feature. `ll_main.c` now reads its argument as a file path first. If
no such file exists, it falls back to treating the argument as a
literal expression. This matches `asnix`'s own long-standing
convention. All 44 differential tests still pass. None of their 44
fixed expressions happens to name a real file.

That fix landed, and `hyperfine --shell=none` removed the remaining
shell overhead from every measurement, not just this one workload.
The full 10-workload suite then ran again. The result: `ll` is faster
than `asnix` on 10 of 10 workloads. The geometric mean across all 10
is 0.65. `ll` runs about 35 percent faster than `asnix` on average,
with no exceptions in this run. A separate check on
`lambda_chain.nix` (2,000 nested calls, excluded from the main 10
for unrelated historical reasons) also finished faster: 1.10 times.

This closes the investigation this file carries since M8. The
two workloads that looked slower were never evidence against the
AOT dispatch redesign. They were evidence of an uncorrected
difference in how the two CLIs take their input, amplified by a
benchmark tool's own default shell wrapping. Once both of those were
corrected, no gap remained to explain.

## M10/M6 update: real nixpkgs evaluation, for the first time

Every result above used `bench/workloads`, a set of small synthetic
microbenchmarks. `ll` was not able to evaluate a real nixpkgs
expression at all until this update. It aborted on the very first
`let` in a real `default.nix`. The error was "op id 5 has no AOT
handler." `ll` had only ported about 34 of 59 `IrOp` kinds. Those
are the ones the microbenchmark suite exercises. Real nixpkgs needs
most of the rest. That list includes `with`, `rec` with
`__overrides`, `//`, string interpolation, `?`, `or`, and dynamic
attribute access.

Six independent agents ported the remaining ops in parallel, each in
its own isolated git worktree. Each agent ported one cohesive group.
One group covered scoping for `with` and `rec`. One covered `//` and
`++`. One covered string and path interpolation. One covered `?`
membership tests. One covered `or` and dynamic select. One covered a
set of five lower-traffic ops. One foundational piece came first. Cell, LazyApp,
and ThunkMap indirection are heap-object aliasing schemes real
nixpkgs's `rec {}` and `mapAttrs`/`zipAttrsWith` use. These now
resolve correctly in both `OP_VAR_REF` and `OP_SELECT`, through the
vendored reference functions `asnix_resolve_cell_chain`,
`asnix_resolve_lazyapp`, and `asnix_resolve_thunkmap`. Each of the
six agents' new code went through a manual check. The check covered
helper signatures, push and pop accounting, and correct use of
`LL_GOTO` versus `LL_NEXT`. Only after that check did each piece get
wired into the shared dispatch table. All 44 differential tests kept
passing throughout.

Two further gaps surfaced once real nixpkgs code actually ran.

The first: `__functor`. An attrset with a `__functor` attribute is
callable. Real nixpkgs's `lib.fix`, `lib.extends`, and overlay
machinery use this constantly. It was out of scope for the whole
M10 port. It is now added to both `ll_op_call` and `asnix_apply`.

The second was a real bug, found with `valgrind`, not by guesswork.
`asnix_apply`'s primop branch did not preserve an invariant that
`builtins_list.c`'s curried-native stages depend on. A curried
builtin such as `elemAt someList` is itself a PrimOp value. It
captures `someList` inside itself. Calling it recovers that captured
value by reading `args[-1]`. That read relies on `op_call.S`'s own
calling convention. That convention always leaves the called value
sitting one slot before its arguments, on `VALSTACK`. A curried
stage can itself be handed to `map`, `filter`, or `genList` as the
callback. When that happens, `asnix_apply` is reached instead, as a
separate call path. It did not provide that same slot. `op_call.c`'s
own primop branch got this right by
accident, through its own `callee_slot` arithmetic. `asnix_apply`
did not. The fix gives it the same self-capture slot.

Verification covered all 74 real package attribute paths in
`asnix`'s own `tests/nixpkgs-hash/attrs.txt` corpus. A few examples:
`hello`, `bash`, `git`, `python3`, `rustc`, `gcc`, `llvm`, and
`ripgrep`. These span compilers, interpreters, CLI tools, and GNU
coreutils. All 74 now evaluate through `ll` and match `asnix`
exactly. The check used `builtins.stringLength` on each package's
real `outPath`, against a real nixpkgs checkout on this machine, not
a fixture. Neither side crashed on any of the 74. `ffmpeg.outPath`
itself, the specific target this update set out to check, evaluates
correctly: a 60-character path, matching `asnix` exactly.

Timing on that same `ffmpeg.outPath` eval used `hyperfine` at 15
runs. The noise was low this time, with a standard deviation under 2
percent, unlike the tiny microbenchmarks above where process startup
noise dominated. `ll` took 946 ms. `asnix` took 831 ms. `ll` is 1.14
times slower here. This is real, substantial work, close to a full
second of actual evaluation, not a few hundred microseconds. This
result is not noise the way the earlier microbenchmark gaps were.
This file's own M10 sections already identified a per-hop register-
placement cost, one this toolchain had no full fix for. This result
is the clearest evidence yet of that same cost. At real nixpkgs
scale, with many thousands of dispatch hops, that cost adds up to a
measurable, genuine slowdown. `ll` still wins on most small,
synthetic workloads.

One thing was not attempted: a full NixOS system closure, such as a
Plasma desktop configuration. That needs the entire NixOS module
system to evaluate correctly, not just the Nix language itself. The
module system means `lib.evalModules`, hundreds of modules, and
assertions. That is a substantially larger undertaking than the
op-porting done here, and it is likely to surface further gaps
beyond this update's scope. This machine also has no local NixOS
configuration to test against.

## Real value printer, ported from asnix's main.c

Until this point, `ll`'s own CLI printed only a raw integer, no
matter what the evaluated result's real type was. The nixpkgs
checks above used proxy values, such as string length and attribute
count, to check correctness, instead of printing the real value.

`src/llvm/ll_print.c` ports asnix's own printer from `src/c/main.c`,
function for function: string escaping, attrset key quoting and
sort order, `<PRIMOP>`/`<LAMBDA>`/`<PRIMOP-APP>` labels for
callables, and `«repeated»` cycle detection for self-referential
attrsets and lists. `ll_main.c` now calls `ll_print_value` in place
of the old integer-only `printf`.

Direct comparison against asnix's own raw output checks byte-for-
byte parity. This is true across strings, lists, attrsets, paths,
booleans, null, floats, primops, lambdas, and a self-referential
attrset. `pkgs.ffmpeg-full.outPath` and `pkgs.ffmpeg-full.drvPath`
now print as real, correctly quoted strings, identical between `ll`
and asnix, not a proxy-checked integer. The 74-attribute nixpkgs-
hash corpus was run again. This time it diffed the two evaluators'
raw `outPath` output directly, instead of comparing string lengths.
It passed 74 out of 74. All 44 difftest cases still pass with the
new printer wired in.

## Closing the real-nixpkgs gap: three fixes, 1.14x down to 1.07x

The prior section measured `ll` at 1.14 times slower than asnix on
`pkgs.ffmpeg-full.outPath`. It gave no explanation beyond a
suspected per-hop register cost. A `callgrind` run on both binaries
on this same expression gave the real breakdown. `ll` executed
7,359,621,183 instructions. asnix executed 6,492,211,413. The gap
was 13.4 percent. Three fixes closed most of it. Each fix was
checked against the full 44-case difftest suite, the 74-package
nixpkgs-hash corpus, and a direct diff of `ffmpeg-full.outPath` and
`drvPath` raw output.

First, `ll_resolve_indirection` (Cell/LazyApp/ThunkMap unwrapping,
called on every variable reference and every force) called three
separate out-of-line C predicate functions. Each of those itself
called a fourth function. asnix's own asm once did the same. An
earlier asnix commit rewrote it to test the tag bits directly
instead. `ll_resolve_indirection` became a header-only inline
function that does the same direct tag-bit test. It falls back to a
real call only for the rare LazyApp/ThunkMap case. A bug surfaced
during this fix. The inlined Cell-chase logic read a stored pointer
as the target value directly. This skipped one level of
indirection. Real nixpkgs evaluation caught the bug immediately:
`with` on a let-bound attrset failed outright. The fix was one
line. This change alone dropped the instruction count to
6,903,932,021 and the wall-clock gap to 1.09 times.

Second, `ll_op_mk_thunk`, `ll_op_lambda`, and `ll_op_mk_attrs` each
made a real, non-tail call to `asnix_arena_alloc` mid-handler. An
inlined bump-allocator fast path removed that call from the common
case. It falls back to the real allocator only on chunk growth or
the memory cap. This gave a smaller further drop, to 6,857,575,434
instructions.

Third, and by far the largest remaining cost: disassembly of
`ll_op_mk_thunk` showed 7 register pushes around its one real call.
asnix's hand-written asm equivalent needed only 2. The cause is the
C calling convention itself. The handler signature passes all five
of its state arguments (ip, env, vs, pool, rs) in registers the
SysV ABI defines as caller-saved. A real call anywhere in a
handler's body forces the compiler to spill and reload all five.
asnix's own asm avoids this. It hand-pins three of its five state
registers in ABI callee-saved slots instead. A real call there
costs only two spills, not five. Clang 19 supports the
`preserve_none` calling convention for exactly this case. This
project's build moved from clang 18 to use it. CPython's own
tail-calling interpreter uses the same mechanism. Adding
`__attribute__((preserve_none))` to every op handler's signature,
with no other code change, dropped `ll_op_mk_thunk`'s register
pushes from 7 to 1. The total instruction count dropped to
6,710,345,987, only 3.4 percent above asnix. Wall-clock time on the
same `ffmpeg-full.outPath` eval dropped to 1.07 times asnix's, down
from 1.14 times at the start of this section. The ten synthetic
`bench/workloads/*.nix` cases still all run faster on `ll` after
these three fixes, the same as before them.

A fourth fix followed the same disassembly comparison further.
`ir_link` (the per-op linking pass run once for every compiled
function body) cost 82,906,578 instructions, 3.2 times asnix's own
26,107,374. The difference: `ll`'s version checked that each op id
was in bounds and had a real handler, aborting with a clear message
otherwise. asnix's own `ir_link` has neither check. It trusts its
compiler to never emit an op id outside the handler table. The full
real-nixpkgs correctness sweep this project already ran (M6/M7)
earns `ll`'s own compiler the same trust. Both checks were removed
to match. The 74-package correctness sweep and all 44 difftest
cases still passed afterward. This checks that no op silently
relied on the removed check. The fix dropped the total
instruction count to 6,693,930,803, 3.1 percent above asnix, down
from 3.4 percent. The ten synthetic workloads still all run faster
on `ll` than asnix, by 1.11 to 1.69 times depending on the
workload.

## Four parallel agents, two more real fixes, down to 1.02x

Four agents ran at the same time, each in its own git worktree, to
look for more of the same class of fix. Each one owned a separate
file set. This kept them from stepping on each other. Each agent had
to pass the full 44-case difftest suite first. Each agent also had to
pass a direct diff of `ffmpeg-full.outPath` against real asnix
output. Only then did it report a change as done.

Two agents found and fixed a real, verified cost. `asnix_env_alloc`
(`src/asm/env.S`) is the Env allocator. Every closure call, every
`let`/`rec` frame, and every `with` scope uses it. It made the same
kind of real, non-tail call mid-handler that `asnix_arena_alloc` did
before its own fix earlier in this document. One agent added an
inlined fast path for it: `ll_env_alloc` in `op_dispatch.h`. It
switched `OP_FRAME_ALLOC`, `OP_CALL`'s closure branch, and `OP_WITH`
to use it. A second agent worked on `closure_helpers.c` at the same
time. It found the same cost in `asnix_apply`'s closure branch and
added its own local copy of the same fast path there. It made one
more fix too. `asnix_force_for_print`'s compiled prologue saved six
registers on every call. It did this even on the common fast-return
path, which needs none of them. The agent moved the slow,
six-register path into its own `noinline` helper function. This let
the compiler shrink the fast path down to two registers, matching
the shape of asnix's own `force.S`.

Both agents touched `src/asm/env_arena.S`. Each needed to export the
same two, previously file-local symbols, and each added that export
on its own. This produced one merge conflict, a duplicated comment,
trivial to resolve. It also produced one duplicate extern
declaration with a mismatched type: `uint8_t *` in one file, `void *`
in the other. The compiler caught this immediately. The fix was one
line. The two other agents reported clean negative results. A sweep
of seven more op handler files found no handler with the old spill
pattern left to fix. A dedicated audit of the `preserve_none` rollout
found no missing attribute and no type mismatch anywhere.

Both real fixes are now merged, and the one post-merge type conflict
is fixed. The instruction count on `ffmpeg-full.outPath` dropped to
6,658,208,843, 2.6 percent above asnix, down from 3.1 percent.
Wall-clock time dropped to 1.02 times asnix's time. This is
essentially parity, within measurement noise. It is down from 1.07
times at the start of this section and 1.14 times at the start of
the whole optimization effort. The ten synthetic workloads are
unaffected. All ten still run faster on `ll` than asnix, by 1.07 to
1.69 times depending on the workload.

## Two more agents, down to a 2.37 percent instruction gap

A second round of two agents, again each in its own worktree, looked
for more of the same. One owned `ir_link` only. The other owned
`closure_helpers.c` only, continuing where the last round's agent on
that file left off.

`ir_link` still cost 66,490,578 instructions, 2.5 times asnix's own
26,107,374, even after the bounds/null-handler check removal earlier
in this document. The remaining cost was structural. `ir_link` made
two full forward passes over every compiled function body's op
array. A `fuse_push_int_runs` pre-pass found runs of four or more
consecutive integer-literal pushes first. A second loop then linked
every op to its real handler address, reading the run flag the first
pass had set. The agent merged these into one single forward pass.
The run-length lookahead only reads op records ahead of the current
position. In a single forward pass, those records are not yet
linked, so folding the lookahead into the main loop is safe. The
agent checked more than the usual two gates. It checked, via
`objdump`, that the compiled function has no call to a separate
fusion helper left. It checked, with a small literal list of seven
integers, that the fused output is still byte-correct. It also ran
one bounded `hyperfine` check on `list_build_large.nix`, the
benchmark most sensitive to a fusion regression. That check found no
change in its margin over asnix.

The second agent did two things to `closure_helpers.c`. First, a
cleanup. The first round's two agents each added an inlined Env-
allocator fast path on their own: one in `op_dispatch.h`
(`ll_env_alloc`), one a near-identical copy local to
`closure_helpers.c` (`ll_env_alloc_fast`). Each agent's isolated
worktree was blind to the other's edit. The second agent deleted the
local duplicate and pointed `asnix_apply` at the shared
`op_dispatch.h` version instead. Second, a further split, in the
same spirit as the earlier `ll_force_thunk_slow` extraction.
`asnix_apply` compiled its three branches (PrimOp, Closure, attrs
`__functor`) as one function. The compiler's single shared prologue
paid for the union of all three branches' register needs: six
callee-saved registers pushed even on the simple, common paths. The
agent split the PrimOp and Closure branches into their own
`noinline` helpers. This let the compiler shrink-wrap the common
path down to a bare tag check and a tail call, matching asnix's own
`apply.S`.

Both fixes are now merged. The instruction count on
`ffmpeg-full.outPath` dropped to 6,645,790,912, a 2.37 percent gap
to asnix, down from 2.6 percent. Wall-clock time across repeated
runs stayed in the 1.02 to 1.11 range. asnix's own absolute time
moved by a similar margin between runs on this shared machine. This
points to measurement noise on a largely I/O-bound eval, not a
regression. The instruction count is the more reliable number here.
The ten synthetic workloads are unaffected. All ten still run faster
on `ll` than asnix, by 1.18 to 1.71 times depending on the workload.

## Correction: every number above compared ll at -O3 against asnix at -O2

Every ffmpeg-full.outPath gap reported above compared `ll`'s build
against asnix's own default build. `ll`'s Makefile sets `OPT` to
`-O3` by default. This applies uniformly to the op handlers and to
the vendored frontend code (lexer, parser, compiler, builtins).
asnix's own Makefile sets `OPT` to `-O2` by default, applied to that
exact same vendored frontend source. Neither Makefile's comments
compare `-O2` against `-O3` directly. asnix's own comment only
measured `-O2` against `-O0`, a different question. Every comparison
in this document up to this point mixed two separate things
together: `ll`'s own dispatch backend improvements, and a real,
unrelated optimization-level gap in shared code. Neither project's
architecture work ever touched that second gap.

The fix: rebuild asnix with `make clean && make OPT=-O3 asnix`.
First check that its own unit tests and `test-e2e` suite still pass
at this optimization level. They do. asnix's own instruction count
on ffmpeg-full.outPath then dropped from 6,492,211,413 to
6,371,026,916. That is about 1.9 percent fewer instructions than its
own `-O2` build, purely from compiling the same C source more
aggressively. Against `ll`'s 6,645,790,912, the honest, matched-
optimization-level gap is 4.31 percent, not the 2.37 percent
reported two sections above. Wall-clock on the same expression has
tighter error bars than earlier runs in this document: plus or minus
roughly 15 milliseconds on about 900-960 milliseconds, instead of
the 25-40 millisecond spread seen before. It puts asnix at 1.06
times faster than `ll`.

This matters for the project's own founding question: does compiling
the dispatch loop through LLVM beat hand-written assembly. The ten
synthetic `bench/workloads/*.nix` cases are small expressions. They
do almost no frontend work, with little to parse or compile. They
mostly measure the dispatch loop itself, the one part this project's
whole backend-replacement effort actually changes. Against the
matched, `-O3` asnix build, `ll` still wins all ten. The margin is
1.12 to 1.79 times, about the same as against the `-O2` build. The
dispatch-layer thesis holds up where it is actually being tested.
`ffmpeg-full.outPath` is different. It spends most of its time in
frontend parsing, compiling, vendored builtins, derivation hashing,
and SHA256. All of that is shared, byte-identical code on both
sides. This project's dispatch-backend work was never going to
change any of it. The honest conclusion: `ll`'s LLVM-compiled
dispatch backend beats asnix's hand-written assembly dispatch loop
on workloads that exercise dispatch, by a wide and consistent
margin. A real-world nixpkgs evaluation is not such a workload. It
is dominated by other costs this project does not touch, and asnix
stays a little ahead there, honestly measured.

## LLVM-specific capabilities: two real wins, three honest negatives

A research pass asked a narrow question. What can `ll` exploit now
that it is compiled by clang, that asnix cannot have at all. asnix
has no compiler reading a profile, since it is hand-written
assembly. Five ideas came out of that pass. Two agents implemented
and measured all of them, each in its own worktree. The result is
two real, kept wins and three honest negative results, all measured
rather than assumed.

Profile-guided optimization is the first real win, and the headline
item. A new, opt-in `make pgo` target runs the standard clang
pipeline. It builds an instrumented binary. It runs that binary over
a training set, the ten synthetic workloads plus ten real nixpkgs
packages chosen for size and domain variety. It merges the resulting
profile data. It rebuilds with that profile fed back in. The
training set deliberately excludes `ffmpeg-full.outPath` itself, the
project's held-out measurement target, so the result is not an
overfitting artifact. Measured on that held-out expression, the
profile-guided binary runs 6,609,724,718 instructions against the
plain build's 6,646,004,511, a real, deterministic 0.55 percent
reduction. Wall-clock showed no reliable signal. `ffmpeg-full.outPath`
spends most of its time in shared frontend and builtin work that
PGO's clang-only scope cannot touch. This is consistent with the
smaller, dispatch-only slice improving by more than the overall 0.55
percent, but that breakdown was not directly measured. Do not treat
it as checked.

`-march=native` is the second real win, and the larger of the two. A
new, opt-in `make ll-native` target builds the clang-compiled files
for this specific machine's CPU instead of a generic x86-64 target.
`objdump` checked this directly: zero AVX register uses in the
generic build, twenty-one in the native one. Early, unpinned
`hyperfine` comparisons on this shared machine were noisy. They were
noisy enough to flip sign between runs, from a misleading 1.09-1.11
times faster to the opposite direction under higher load. Pinning
both sides to the same CPU core with `taskset`, and testing both
orderings, fixed this. The result is a consistent, real 1.04 times
wall-clock improvement on `ffmpeg-full.outPath`. This comes with an
explicit, documented tradeoff. The binary is tied to the build
machine's own CPU. It can fail outright on a different one. It is
never the default for that reason.

Thin LTO, scoped only to the clang-compiled files, gave no
measurable win, roughly 1.03 times slower, within noise. The agent
reverted it and left a plain negative-result note in the Makefile.

`__builtin_expect` branch hints, applied to every unambiguously-cold
branch across the op handlers (fatal errors, stack overflow checks,
type mismatches), gave a clear, repeatable regression. 6,661,804,998
instructions against a freshly measured baseline of 6,645,949,556, a
0.239 percent increase, checked twice. Bisection traced almost all of
it to four small helpers. Those helpers are inlined into nearly
every single op handler: the valstack and retsp overflow checks, and
the arena and env allocator fast paths. The likely explanation: if a
branch's taken side calls a `noreturn`-attributed function, clang
already treats that branch as cold by default. The explicit hint was
redundant on its own for that reason. Its earlier-pass lowering then
fought with that later, more precise analysis, once inlined this
pervasively under the already tight register pressure `preserve_none`
creates. The agent reverted the change in full. Nothing from it is
left in the tree.

A sixth idea came from a separate request, not this research pass's
own list, and went through two further implementations. The idea:
fuse consecutive identical ops at `ir_link` time, the same technique
the existing `OP_PUSH_INT_RUN` fusion already uses. One attempt
generalized this to attribute-selection chains. The other generalized
it to the `OP_FRAME_GET`-then-`OP_MK_ATTRS` sequence every `rec {}`
literal compiles to. Both were implemented correctly. Both passed
the full correctness suite plus dedicated edge cases. Both measured
as small, repeatable regressions rather than wins: 10 million and
10.4 million more instructions on `ffmpeg-full.outPath`. The select-
chain case traced to `ir_link`'s own lookahead cost. That cost runs
unconditionally on every `OP_SELECT`, fusing or not, and outweighs
the runtime savings from the cases that did fuse. The attrset case
has no fully checked cause. One plausible, unchecked guess: the
larger, loop-bodied replacement handler interacts less favorably
with `preserve_none` than the sum of the small handlers it replaces.
Neither change is merged. The attrset attempt's worktree stays at
`/tmp/ll-worktree-fused-attrs` (branch `fused-frame-get-mk-attrs`)
for anyone who wants to dig into the real cause before trying again.
The select-chain attempt was fully reverted and removed.

The lesson across all five of these attempts: the `OP_PUSH_INT_RUN`
fusion's success does not generalize by analogy alone. Every one of
these five ideas looked plausible from first principles. Two held up
under measurement. Three did not, for three different, mostly-
understood reasons. Measure before trusting any of them.

## M7: the real conformance sweep, run against `ll` for the first time

`ll` never had a working target for the external numtide/nix-
conformance corpus. M7, the conformance milestone, stayed pending
for this reason, not because the work was done and unremarkable.
Running it for the first time (corpus cloned to /tmp/nix-conformance)
gave a bad first result: 335 pass, 323 fail, 148 skip, against
asnix's own 655 pass, 3 fail, 148 skip on the identical corpus.

The dominant cause, 271 of the 323 failures alone: `ll_main.c`
hardcoded the literal string `/tmp` as the base directory for every
relative `import`. It also passed `NULL` for the file path that
position-tracking builtins read. It did this regardless of which
file was actually being evaluated. Every `eval-okay-regex-*` case
failed on the exact same broken `import ./corpora/regex.nix`. The
fix replicates asnix's own `eval_file.c` logic exactly: make the
real file path absolute and canonical, then take its directory for
imports and the path itself for position tracking. This alone
brought the count to 639 pass, 19 fail, 148 skip.

Before chasing the rest, a direct `diff` checked every vendored file
in `src/c/` (derivation.c, string_context.c, builtins_compare.c,
tryeval.c, and the rest) against asnix's own copy. They are byte-
identical, down to the single header-comment line that naturally
differs. This mattered for how the remaining 16 failures got
investigated. None of them can be a bug in the shared, vendored
logic, since that exact code already works in asnix. Every one has
to be in `ll`'s own glue code, the parts that replaced asnix's hand-
written dispatch layer.

Three agents split the 16 remaining failures by symptom, each in its
own worktree. Two of them worked on separate clusters in parallel.
They independently found the exact same root cause. `root_env.c`,
`ll`'s own code, still wired the TOP-LEVEL root environment's
`derivation` slot to a stripped-down bootstrap substitute. That
substitute exists only to break a self-referential cycle inside the
real Nix-level `derivation` wrapper's own construction, not for
ordinary use. It behaves exactly like plain `derivationStrict`: no
`outPath`, `drvPath`, `outputName`, `outputs`, or `type` fields. No
check this project had run before caught the gap. An imported file's
root environment already used the real wrapper correctly. Only a
program's own top-level `derivation {...}` call, the exact shape
most of the conformance corpus uses, went through the broken
substitute. A one-line fix, in slots 0 and 8 of `ll_make_root_env`,
resolved all 15 of those two agents' assigned cases at once. String
coercion, delayed `with` scoping, and every derivation-output and
equality case all trace back to the same missing fields.

The third agent fixed a narrower, one-case bug. A thunk can fail its
first forcing attempt inside a `builtins.tryEval`. If something
forces that same thunk again, inside a second `tryEval`, the second
`tryEval` must catch the re-raised failure, not crash the whole
process. asnix's own hand-written assembly does this by calling
`asnix_tryeval_raise_or_fatal` instead of a flat fatal exit on that
path. Two of `ll`'s own ported files, `op_var_ref.c` and
`closure_helpers.c`, called the flat fatal exit directly instead.
This never gave the second `tryEval` a chance to catch it. Matching
the asm's own behavior fixed it.

Merging the two root_env.c fixes produced exactly one conflict, and
it was trivial: both agents wrote the identical code change,
independently, and only their explanatory comments differed enough
for git to flag it. The final result, re-run against the full
external corpus after merging all three fixes: 655 pass, 3 fail, 148
skip, an exact match for asnix's own documented rate on this corpus.
The 3 remaining failures are asnix's own pre-existing, already-
documented position-tracking gap, not a new one. All 44 difftest
cases, the 74-package nixpkgs-hash sweep, and a byte-identical
`ffmpeg-full.outPath` diff against asnix stayed green through every
step of this work.

## The attrset-allocation gap with iets: two real fixes, two honest negatives

`iets`, a separate Rust Nix evaluator, uses about half the peak
memory of `ll`/`asnix` on the same real workload,
`pkgs.ffmpeg-full.outPath`. This section explains why, using memory
instrumentation already present in both projects
(`ASNIX_SHOW_STATS=1` and `IETS_STATS=2`) plus `/usr/bin/time -v` for
peak RSS. `iets` is not a correctness reference here. It is a
performance comparison only.

Attrset allocation accounts for 65.7MB of the gap. `asnix`/`ll`
builds 119.9MB of attrset entries on this benchmark, where `iets`'s
equivalent categories sum to 54.3MB. Most of this gap traces to the
`//` operator. Three agents, each in an isolated worktree,
investigated three separate parts of the gap in parallel.

### Fix 1: permanent garbage in the lazy-union materialize path

A chain of small `//` operations can defer into a lazy union (asnix's
own M88 design, see `attrs_update.c`). Reading the union's full set
of keys later calls `asnix_bshared_attrs_resolve`. This function
merges the union's leaves bottom-up, in pairs, and halves the work
list each round. Every round's output used to go into a real,
permanent `asnix_arena_alloc` call, even though only the FINAL
round's output is ever read again. On the real benchmark, this
wasted 1.5 million entries, about 18.5MB, checked by counting.

An earlier attempt this session replaced the pairwise merge with a
single-pass merge across all leaves at once, a K-way merge. The
K-way merge eliminated the garbage cleanly, -3.4% peak RSS. But it
cost more raw instructions, +2.4%. One pass scans every leaf cursor
for each output entry. That does more comparisons than the old
two-way merge, for workloads with few leaves. The K-way version was
reverted.

The fix that worked keeps the exact same two-way merge algorithm. It
redirects every round except the last into one of up to six
dedicated, transient scratch buffers, plain `malloc`, not the
arena. Each round gets its own buffer. It never ping-pongs between
two buffers. An entry carried over unmerged from an earlier round
must stay valid into a later round. Reusing a buffer across
non-adjacent rounds corrupts that entry. The last round writes
straight into the one real, correctly-sized, permanent allocation.
The function frees all scratch buffers before it returns.

Two further instruction-count regressions had to be fixed before
this design paid off. The merge helper needed
`__attribute__((noinline))`, to match how `asnix_attrs_eager_merge`
is already compiled. Without that attribute, the compiler folded the
helper into its caller, and pushed its 12-byte `memcpy` calls into
slower `__memcpy_chk` calls. The last round's extra copy, from
scratch buffer into the final allocation, was removed by writing
that last round directly into the final allocation instead. Final
measured result: instructions +0.015%, noise level. Peak RSS
-3.57%, about 18.8MB, matching the predicted garbage almost exactly.

The fix passed four gates:

- all 44 difftest cases
- the 74-package nixpkgs-hash sweep
- a byte-identical ffmpeg diff against asnix
- the full external conformance sweep (655 pass, 3 fail, 148 skip,
  unchanged)

It also passed several hand-written edge cases: 1, 2, 3, 5, 64, and
70-leaf unions, duplicate keys across leaves, and empty-attrset
unions. The merge landed on master at commit 8d5ef22.

### Fix 2: a redundant copy in removeAttrs and mapAttrs

A separate agent surveyed every attrset-producing operation other
than `//`: literal `{...}`/`rec {...}` construction, `removeAttrs`,
`mapAttrs`, `listToAttrs`, `filterAttrs`, `intersectAttrs`, and
`zipAttrsWith`. Most of these were already tight. Literal
construction already writes straight into its final buffer.
`listToAttrs`, `filterAttrs`, and `intersectAttrs` use
`malloc`-and-free scratch, which the never-free arena does not
retain.

`removeAttrs` and `mapAttrs`, in `builtins_attrs.c`, both built a
symbols-and-values scratch pair in the permanent arena. Each one
filled the pair, then handed it to a helper that allocated a SECOND
permanent buffer and copied every entry across again. On the
never-free arena, the first buffer became permanent garbage the
moment the copy finished. The fix writes directly into one real
allocation in both cases. `removeAttrs` allocates for the worst
case, nothing removed, and shrinks the recorded count afterward.
`mapAttrs`, a plain one-to-one map with no over-allocation question,
writes straight into one final buffer on both its code paths.
Measured result: instructions -0.10%, an improvement. Peak RSS
-1.50%, about 7.9MB. The fix passed the same four gates and landed
on master at commit 69fddcc.

A two-pass variant of the `removeAttrs` fix was also tried: count
survivors first, then allocate exactly that size, with no worst-case
tail slack at all. It cost more instructions, +0.267%, for less
memory benefit than the single-pass version above. The two-pass
variant was discarded.

### Two honest negatives

A third agent measured whether `asnix_attrs_eager_merge`'s own
direct `//` path wastes allocation slack the same way the
materialize path did. This path handles the 92,218 calls that do not defer into a
lazy union, about 52MB. It does not waste meaningful slack: only
1.8% of this path's own allocated entries go unused to duplicate
keys, about 940KB total. `iets`'s own equivalent function computes
an exact output size first, with a counting pre-pass, before it
allocates. That is a genuine architecture difference from `ll`'s own
worst-case-then-slack approach. The same two-pass approach, built in
`ll` and
measured honestly, cost +0.50% more instructions for only -0.26%
RSS, a worse trade than the existing approach. It was reverted, not
merged.

A separate background task ported `iets`'s own curry-chain-
flattening mechanism into `ll_op_call`. The mechanism binds every
argument of a nested single-argument lambda chain directly, without
allocating an intermediate closure for each stage. It was correct,
with 220,369 fast-path hits, checked directly, on the real
benchmark. But it
produced no measurable speedup: 0.008% on instructions, no
significant change on wall clock. The avoided closure allocation is
small next to the environment allocation that still happens the
same way on both paths, which dominates the real cost. It was not
merged.

### Combined result

Merging fix 1 and fix 2 onto master, with no conflicts since they
touch different files, produced a combined result on the real
benchmark: instructions 6,646,404,613 to 6,639,065,037, -0.11%, a
small improvement, not a regression. Peak RSS 527,260KB to
500,324KB, -5.1%, about 26.3MB of the original 65.7MB attrset gap
with `iets`. All four correctness gates stayed green after the
merge.

A fuller accounting, in the next round below, found a different
number: 62.4MB, not 39.4MB as first estimated here. The asnix
census this section compares against counts only two of the real
attrset-allocation sites. Three more sites exist: lazy-union leaf
headers, dynamic `${...}` attribute insertion, and `removeAttrs`/
`mapAttrs`. That census misses all three, on both sides of the
comparison equally, but they are real. Counting every site gives
`ll`'s true total as 116.6MB before the next round's fix.

## Matching iets's exact `//` heuristic: the single biggest fix

A full accounting counted every attrset-allocating site, not just
the two the asnix census counts. It found `ll`'s real total at
116.6MB against `iets`'s 54.3MB. That is a 62.4MB gap. The next step compared two designs: `asnix`'s
own `//` logic, and `iets`'s exact algorithm. This went past
`iets`'s MAX_LEAVES constant, already tried and rejected earlier
this session. The comparison found three real differences. Each
one is a free win on real nixpkgs code.

1. `iets` returns the other side unchanged, with no allocation, for
   `x // {}` or `{} // x`. `asnix`'s own design had no such check.
   Real nixpkgs and stdenv code hits this constantly: optional
   overrides, `lib.optionalAttrs false {...}`, and default-argument
   attrsets.

2. `asnix`'s own `asnix_attrs_update` resolved the RHS before it
   even checked whether to defer. `iets` never forces the RHS up
   front. Overlay composition often passes a still-deferred union
   as the RHS of a `//`. `iets` keeps that union's own leaves
   unresolved instead of flattening it right away.

3. On leaf-budget overflow, `asnix`'s own design resolved the whole
   left side and ran a full eager merge of both sides. `iets`
   flattens only the left side, down to one leaf, and retries as a
   fresh, short union. A naive copy of `iets`'s MAX_LEAVES=8
   constant onto the OLD restart strategy did not have this piece.
   That is likely why the earlier attempt measured worse: it paid
   the old, expensive restart eight times more often, with no
   cheaper alternative in place. The cheap restart makes the full
   two-sided merge rare.

Porting all three together, not each alone, closed most of the
gap. On the real benchmark, `ll`'s true attrset-byte total fell
from 116.6MB to 84.6MB. The gap with `iets` fell from 62.4MB to
30.3MB. Instructions rose by 0.345%, a real, reproducible cost, not
noise. Peak RSS fell 6.3%, and minor page faults fell 6.0%. Wall
clock (hyperfine, 12 runs each) showed no measured change: 999.0ms
to 978.5ms, well inside the run-to-run noise band.

This result stands apart from every other memory-for-instructions
trade tried this session. The K-way merge earlier in this document
cost 2.4% more instructions for a 3.4% RSS gain. This same round
tried, and again rejected, an exact-size two-pass version of the
direct `//` path. That version cost 0.50% more instructions for
only 0.26% RSS. Here the ratio runs the other way, about 18 to 1 in
memory's favor. The metric that actually matters, wall clock, shows
no cost at all. The fix was committed.

All four correctness gates passed. So did several hand-built edge
cases: empty operands on both sides, nested-union priority with
duplicate keys spanning both sides' own sub-leaves, the eight-leaf
overflow restart, and an unbounded RHS size. `asnix`'s own design
capped the RHS at 16 entries for deferral. `iets`'s design, now
ported, has no such cap.

## The materialize path: where the rest of the gap lives

With the fix above in place, a full breakdown by true origin
covered every real attrset-constructing call site, not just the
asnix census's two categories. It split `ll`'s remaining 84.6MB
this way:

- True literal syntax, `{...}` and `rec {...}`: 12.7MB.
- The real `//` eager-merge fallback: 7.5MB.
- Lazy-union leaf headers, deferred and not yet materialized: 2.5MB.
- Dynamic `${...}` attribute insertion: 5.3MB.
- `removeAttrs` and `mapAttrs`: 9.4MB.
- The derivation-result attrset itself: 0.2MB.
- The lazy-union materialize path, the final flatten that runs once
  something needs a union's full key set: 41.3MB.

The materialize path alone outweighs every other category combined.
It is not one single cause. Breaking its 30,304 real flatten events
down by the call site that triggered each one found five distinct
causes, none dominant alone:

- 14.3MB from `asnix_attrs_update`'s own leaf-budget-overflow
  restart, flattening the accumulated left side of a long `//`
  chain. A further 7.5MB comes from the same function's eager-
  merge fallback flattening its right side. Both costs are a direct,
  unavoidable part of the same heuristic change above. `iets` pays
  this too, already counted inside its own 23.1MB `AttrsUpd` total.
- 7.1MB from dynamic `${...}` attribute insertion, which needs a
  flat base to insert into.
- 7.0MB from `derivation.c`'s own two highest-risk call sites. This
  codebase's own comments already flag both sites. Heavy `//`
  chaining (stdenv's mkDerivation defaults merged with overrides)
  meets a real consumer that needs the full result right there.
- 2.8MB from a handful of `mapAttrs` calls, 27 in total, each one
  over a very large attrset averaging more than 8,000 entries.
- 2.7MB from the shared count-check helper behind `attrNames`,
  `attrValues`, `hasAttr`, `getAttr`, and `removeAttrs`.

Restricting the comparison to what `//` itself causes, the leaf
headers, the eager fallback, and the two `asnix_attrs_update` rows
above, gives 31.7MB. Against `iets`'s whole `AttrsUpd` total of
23.1MB, that is a real but much smaller 1.37x gap, not the 2.2x the
unrestricted total suggests. The remaining 19.3MB of the
materialize total comes from other operations: `mapAttrs`, the
attrNames family, derivation processing, and dynamic insertion.
Each one reads the full contents of a union some earlier `//`
built.
This cost belongs to any lazy-union design, `iets`'s included. This
project has no way to measure `iets`'s own equivalent split without
changing `iets`'s own source, which is out of scope here.

No further fix was attempted past this point. Every lead left
standing is a smaller version of a trade already tried and rejected
twice this session: tune the leaf-budget threshold further, or stop
caching a one-off materialize result. Both carry a shrinking return.

## Two safe probe-based fixes, and a wider look past attrsets

A follow-up round re-examined the remaining gap with a direct goal.
It aimed to match `iets`'s real wall clock on `ffmpeg-full.outPath`,
not just its attrset byte count. A fresh three-way hyperfine run, 25
samples, found `iets` a real, reproducible 1.17x to 1.28x faster
than `ll` end to end. This holds even though `ll` runs fewer raw
instructions: 6.66 billion against `iets`'s 7.49 billion, on the
same callgrind run. Instruction count alone does not explain this
workload.

`ll`'s own memory footprint is the likelier cause. Rebuilding `ll`
from before today's attrset fixes, then comparing wall clock, found
those fixes already bought a real 8.5% speedup: 1.122s down to
1.026s. Two of those fixes cost instructions on paper. From this
point on, this round judged every change by wall clock, not
instruction count.

Two more real, safe fixes came out of this round. Both follow the
same pattern `asnix_bshared_attrs_probe` already established: a
single-key lookup never needs the whole set materialized.

`asnix_collect_input_refs`, in `derivation.c`, is one of the two
"highest-risk" materialize call sites named above. It only ever
reads three known keys, `type`, `drvPath`, and `outputName`, to
decide whether a value is a derivation. It used to resolve the
whole set first. It now probes for all three keys directly instead.
When the probe fails to find all three keys, it falls back to the
original resolve-and-scan. That fallback case is rare: a value that
is not a derivation at all, which genuinely needs the full
json/`__toString`/`outPath` handling. Measured result: instructions
down 0.072%, a real improvement. Peak RSS stayed flat. Most of this
call site's hit volume turned out to be already-flat attrsets
paying an O(n) scan cost, not a union materialize. The fix passed
all four correctness gates and was merged.

`hasAttr` and `getAttr`, in `builtins_attrs.c`, had the identical
problem one level down. Both routed through a local `attrs_find`
helper. That helper called `attrs_count` first, a full resolve just
to bound a binary search. Both builtins now call
`asnix_bshared_attrs_probe` directly instead. `attrs_find` is now
dead code and was removed. The effect on `ffmpeg-full.outPath`
itself sits inside measurement noise, since that benchmark does not
lean heavily on these two builtins. The fix is correct regardless.
This exact gap was already named in `asnix_bshared_attrs_probe`'s
own header comment, as a deferred scope cut from an earlier
milestone. Closing it costs nothing and helps any workload that does
lean on these two builtins. The fix passed all four correctness
gates and was merged.

A wider census compared every object category, not just attrsets,
between `asnix`'s own stats and `iets`'s. It found two things worth
recording. First, environments sit at near parity already. `ll`
creates more of them than `iets`, 1,038,688 against 780,378, but
each one is smaller on average. The two totals land within half a
percent of each other: 18.82MB against 18.92MB. No fix is needed
here.

Second, thunks show a real, unexplained 7.7MB gap. `asnix` creates
1,933,944 of them at a fixed 16 bytes each. `iets` creates 481,336
fewer thunk-family objects, at the same 16 bytes each. The gap sits
purely in how often each system decides to defer something, not in
how big each deferred unit is. This round did not find the root
cause. `ll`'s own compiler already runs a mature, well-tested
thunk-elision pass, `is_thunk_free_literal`. It covers constant
literals and folded binary operations. A separate pass,
`try_cell_ref`, covers bare variable references. Closing this gap
further needs tracing real call sites, not guessing at one.

Third, and the largest single number in this wider census: `asnix`
allocates 894,153 Cell objects, 16 bytes each, 14.3MB in total, for
bare variable references that resolve lexically. This is a real,
deliberate design (M92 in this codebase's own history), built to
preserve real Nix's own slot-identity behavior for `==` and the
printer. `iets`'s own census reports no equivalent category at all.
Two explanations are possible. `iets` can achieve the same aliasing
with zero allocation, a plain pointer copy into the new binding's
slot rather than a wrapper object. Or `iets` can skip the same
guarantee entirely. This round did not pursue a fix here. The two
probe-based fixes above only change how a lookup finds data.
Changing how variable aliasing works risks a real behavior
regression in verified `==`/printer semantics, not just a perf
cost. It needs its own careful investigation before any change.

## A tempting, unsafe lambda optimization, caught and reverted

The wider census above also found a thunk gap. `asnix` creates
1,933,944 thunks, each a fixed 16 bytes. `iets` creates 481,336
fewer thunk-family objects, at the same size. This round looked for
the cause in `is_thunk_free_literal`, the compiler's existing pass.
That pass already skips thunk allocation for literals known to be
pure and side-effect-free: integers, floats, booleans, null, folded
binary operations, plain strings, plain paths, and the empty list.
Lambda literals are not on that list, yet evaluating a bare `x:
body` expression is also pure and side-effect-free. It only builds
a Closure, capturing the current environment. It never touches the
lambda's own body. Adding `AST_LAMBDA` to this list looked like a
free win: skip the thunk, build the closure directly, and save one
allocation per lambda-literal binding.

A check against real Nix's own source raised a warning sign first,
not proof the idea was safe. cppnix's `Expr::maybeThunk` has
overrides for `ExprVar`, `ExprInt`, `ExprFloat`, `ExprString`,
`ExprPath`, and the empty-list case in `ExprList`. It has no
override for `ExprLambda`. `ExprLambda::eval` itself is a single
line there, matching the lambda case's own cheap cost reasoned
above. Real Nix never added this specific optimization.
This project's own `is_thunk_free_literal` already goes past
cppnix's exact set in one place, folding constant binary
operations. The missing cppnix precedent here was treated as a
caution, not a blocker, and the change was implemented and tested.

The fix passed 44/44 difftest, the full nixpkgs-hash sweep, and a
byte-identical ffmpeg diff. It then failed one external conformance
case, `eval-okay-eq-slot-attrs.nix`, newly broken. A minimal repro
found the exact mechanism. `builtins.attrValues a == builtins.
attrValues a` must return true, where `a.g` is a bare lambda
literal. Before this fix, `g`'s slot held an unforced Thunk, read
lazily and left unforced by `attrValues` on both sides. `==`'s own
comparison catches two references to the identical, still-unforced
Thunk pointer before it forces either side, and returns true right
away. Real Nix's own function comparison has no such escape once a
value is forced: two function values compare false regardless of
identity. The thunk-pointer shortcut is not a performance nicety
here. When the slot holds a function, this is the only path through
which two lazy reads of the same slot can ever compare equal. Pre-
building the Closure removes the thunk that shortcut depends on. So
both reads fall through to the general, always-false function rule
instead.

This is exactly the trap cppnix's own missing `ExprLambda::
maybeThunk` override avoids by construction. Real Nix never built
this specific optimization, and the reason is now clear. Its own
slot-identity shortcut depends on a real, unforced Thunk existing
at the shared slot. This change removed exactly that thunk.

The fix was reverted. All four gates returned to the documented
baseline right after: 655/3/148 conformance, 44/44 difftest, 74/74
nixpkgs-hash, byte-identical ffmpeg diff. Future work must not
retry thunk-elision for any literal kind without first checking one
thing. Does `==`'s own comparison, for that kind, ever rely on
comparing the UNFORCED representation for slot-identity purposes.
That risk applies to any value kind whose forced-value comparison
is not a simple value compare. Functions are one such kind. Lists
and attrsets already compare structurally by value once forced, not
by identity, so they are not known to share this exact trap. Each
new candidate still needs its own check.

## The Cell gap, closed out: an architecture choice, not a missed trick

The earlier census flagged `asnix`'s 894,153 Cell objects, 14.3MB,
as the biggest open lead. `iets`'s own census reports no matching
category at all. This round checked cppnix's own source to settle
the question. `ExprVar::maybeThunk` there returns `state.
lookupVar(&env, *this, true)` directly: the SAME `Value*` pointer
already sitting in the referenced slot. No new object. No copy of
any kind.

This works in cppnix, and in `iets`, for one specific reason. Both
represent a Nix value as a stable, heap-allocated struct, referenced
everywhere by pointer. Aliasing a variable there is just copying
that one pointer into a new slot. `asnix` and `ll` represent a Nix
value differently: an 8-byte tagged word, copied by value everywhere
this codebase touches it. Nothing here gets referenced through one
shared, stable address the way cppnix's `Value&` is. Aliasing a
variable under this design needs a real extra object: something
with its own stable address, holding a pointer to the target slot,
that every force chokepoint can recognize and chase through. That
object is the Cell.

This closes the question properly, not just defers it. The Cell gap
is not a missed optimization `iets` happens to have and `ll` does
not. It is the direct, structural cost of a whole-system
representation choice made long before this investigation started.
The same choice underlies M118's own compressed-pointer `Env`
design, and this whole dispatch-loop architecture builds on it.
Removing the Cell means rewriting how every value in this codebase
is represented and passed around, not a targeted fix to one call
site. That is well outside the scope of this round's work, and
likely outside the scope of any single round: such a change touches
every op handler, every builtin, and every heap-object kind at
once. No further investigation is planned here unless a future
round is explicitly scoped to that scale of change.

## Correction: callgrind's instruction count misled this document once

Every earlier section here trusted callgrind's instruction count as
the primary speed metric. That trust was correct for every
`ll`-vs-`ll` and `ll`-vs-`asnix` comparison in this file. Both sides
are single-threaded there, both share the same architecture, and
callgrind's simulated count tracked real hardware closely. It was
not correct for the one `ll`-vs-`iets` comparison reported earlier.
Callgrind showed `ll` running fewer instructions than `iets`, 6.66
billion against 7.49 billion, while losing on wall clock. This
document used that gap to argue that instruction count alone did
not explain the loss.

`perf stat` tells a different story. This round pinned it to one
physical core, to rule out this machine's hybrid P-core/E-core
scheduling as a confound. Three repeated runs each showed under 1%
run-to-run variance.

- Instructions: `ll` 5.82 billion, `iets` 5.28 billion. `iets` runs
  fewer, not more, on real hardware.
- Cycles per instruction: `ll` 1.68, `iets` 1.95. `iets` also uses
  each cycle more effectively.
- Page faults: `ll` 123,336, `iets` 26,749 to 75,915 depending on
  measurement context. `iets` faults four to five times less.
- Cache-miss rate: `ll` 48.9%, `iets` 44.1%, on top of `iets` also
  making 9% fewer cache references in absolute terms.
- Branch-miss rate: `ll` 2.75%, `iets` 2.32%.

`iets` wins on every one of these, not just wall clock. The earlier
claim that `ll` is instruction-cheaper but memory-expensive was
wrong. Valgrind's callgrind simulates a generic pipeline. Real
silicon schedules `iets`'s own multi-threaded runtime differently
(mimalloc background threads). `iets`'s own stats report 3 threads
even under `--workers 1`. The discrepancy
is specific to measuring a multi-threaded binary under callgrind.
It is not a general flaw in this project's own callgrind-based
methodology. The `ll`-vs-`asnix` work stays single-threaded on both
sides and remains trustworthy.

The real picture is simpler than the one reported earlier. `iets`'s
lead is not "wins on memory, loses on raw computation." It wins on
computation too. The likely common cause is the same memory-
footprint gap this whole investigation chases. Writing fewer bytes
needs fewer store instructions. Touching fewer pages needs fewer
page faults. A smaller working set needs fewer cache misses, which
raises the instructions-per-cycle rate on everything else. Two
numbers point the same direction as the attrset, thunk, and Cell
gaps already measured in this document: peak RSS (`ll` 468MB,
`iets` 308MB, a 1.52x gap) and the page-fault count. Closing more
of the remaining memory gap likely closes part of the real
instruction and cycle gap too. It does not trade against it the way
this document's own earlier framing assumed. Future `ll`-vs-`iets`
comparisons must use `perf stat`, pinned to one core. Callgrind
stays the faster, cheaper tool, but only for same-architecture
`ll`-vs-`ll`/`asnix` comparisons.

## Two real lexer wins, found by a real CPU profile

`perf stat` now works on this machine. This round also tried `perf
record` for the first time, for a real, sampled hot-function list.
Earlier work in this document relied on callgrind alone. The result
named two self-time leaders this document had never checked:
`asnix_intern` and `lexer_next`, at 4.41% and 4.38%. Every earlier
section here targeted evaluation, not parsing. This was new ground.

`lexer_next`'s own `lex_normal` function skipped a line comment with
a byte-at-a-time loop. That loop stopped at `\n` or the string's own
`\0` terminator. Glibc's `strchrnul` already does exactly that, in
one SIMD-accelerated call. The lexer's block-comment case got the
same treatment one level removed. It jumps to the next star with
`strchrnul`, then checks for the closing slash. A star that turns
out to be part of the comment's own text, not its end, falls back
to a byte-at-a-time retry from that point.

A second fix in the same function batched whitespace. The lexer
used to advance one byte per space character. Each step re-ran its
own end-of-input and comment checks. Real nixpkgs source is mostly
indentation and blank lines. A tight loop over the lexer's own
existing character-class table now skips a whole run of whitespace
in one pass.

Both fixes passed all four correctness gates. Both also passed
hand-built edge cases: stars inside block comments, multiple
consecutive stars, a comment with no trailing newline at end of
file, and the unterminated-block-comment error path. Measured on
`ffmpeg-full.outPath` with callgrind instructions: comment skipping
cut 0.055%. Whitespace batching cut a further 0.41%, the larger of
the two. That order matches real source, where whitespace is far
more common than comments.

Three other hot functions from the same profile were checked and
found already near their practical ceiling. None needed a further
fix. `asnix_intern`'s own hashing was already rewritten word-at-a-
time in an earlier milestone, measured directly against `iets`'s
own hasher as the reference. `buf_put_escaped`'s character scan
already uses a SWAR bit-twiddling trick. That trick was ported
directly from `iets`'s own escape-finder and checked with a fuzz
test. An earlier milestone already caches its result across a
derivation's three ATerm-building passes. `make pgo`, rebuilt fresh
against every fix in this document so far, reproduced the same
real-but-small instruction win recorded elsewhere in this file:
callgrind instructions down 0.72%. It also reproduced the same
unreliable wall-clock result on this one benchmark. `ffmpeg-
full.outPath`'s own hyperfine run came back slightly slower, not
faster, under PGO. PGO stays an opt-in build target, not the
default, for the reason already on record.

One more lead was found but not pursued. `malloc`'s own cost traces
mostly to `asnix_prim_derivation`'s per-derivation escape cache.
That cache is deliberately keyed by pointer, so it only helps
within one derivation's own three-pass build. A cache keyed by
string content instead of pointer, shared across derivations,
is a candidate target: `system`, for one value, repeats across
many derivations. Such a cache needs real hashing and comparison
work of its own. It also needs real correctness care: this file's
own history already records subtle lifetime bugs in this exact
area. The bound on the possible win is small either way, about
1.2% of instructions at most, since that is `malloc`'s whole
measured share. This is left here as a candidate for a future
round, not attempted in this one.

## FormalsMap: one shared descriptor for required lambda-pattern formals

The thunk-gap investigation found one dominant cause. Pattern-lambda
formal binding, the code that binds each field of a `{a, b, ...}:`
function pattern, builds 25.6% of all thunks on `ffmpeg-full.outPath`.
Real nixpkgs code calls this path 46,719 times, with an average of
10.6 formals each. Every required (no default) formal got its own
Thunk object, even though its value is always the same fixed
operation: select the field from the call's own argument, then
force it.

FormalsMap replaces one Thunk per required formal with one shared
descriptor per call. It reuses the pointer-tagging trick ThunkMap
already uses for `mapAttrs`: a packed 8-byte Value encodes the
descriptor's address plus the formal's position within one word.
The first read of a given formal resolves it from the call's
argument attrset. It then writes the result back into the same
slot it came from. A later read of that same slot sees the plain
ground value directly, with no extra step. This mechanism is
`ll`-only. `asnix` has no equivalent, since this gap does not exist
there in the same shape.

Two bugs surfaced while building this. The first crashed every real
pattern-lambda call. The op that builds the descriptor read the
call's argument straight from its env slot. That slot still held
the lazy thunk. An earlier step already forced that thunk on the
value stack, for its own separate purpose. It never wrote the
forced value back into the env slot. So the descriptor stored a
stale pointer, not the real attrset. The fix reads that slot the
same way an ordinary variable read resolves an already-forced
thunk, before building the descriptor. The second bug was in the
printer. Its cycle-detection key for a FormalsMap read reused the
exact address an enclosing Cell reference already used, one step
up. So even a first, non-repeated read printed as `«repeated»`.
The fix uses the packed Value's own bits as the key instead, a
value that never collides with a bare pointer.

The fix passed four correctness gates:

- all 44 difftest cases
- a 14-case hand-built pattern-lambda file, matched byte-for-byte
  against `asnix`
- all 148 nixpkgs-hash attribute/field pairs
- a byte-identical `ffmpeg-full.outPath` diff against `asnix`

That hand-built file covers required fields, optional fields,
letrec-style forward visibility, `...`, `@` aliases, unused fields
never forced, and recursion through required formals.

Measured on `ffmpeg-full.outPath`, against the same workload before
this change (three runs each, under 1% variance):

- callgrind instructions: down 3.42%
- pinned `perf stat` instructions: down 5.06%
- pinned `perf stat` wall clock: down 27.5%
- peak RSS: down 11.5%, about 54 MB
- minor page faults: down 10.9%
- cache misses: down 8%

Major page faults stayed at zero on both sides. Branch misses stayed
about flat. Every measured number moved the same direction, with no
correctness loss, so this is now a permanent feature, not a
prototype. See `heap.h`'s `ASNIX_FORMALSMAP_MARKER` comment for the
full design, and `op_misc.c`'s `ll_op_mk_formalsmap` for the
handler.

## A tried and reverted idea: a cross-derivation ATerm escape cache

The escape cache in `aterm.c` only shares work within one
derivation's own three passes (masked, real, cache-hash). It is
keyed by source pointer. Many strings repeat across derivations too,
such as `system = "x86_64-linux"`. A content-keyed, process-lifetime
cache looked like a way to share that work across the whole eval,
not just one derivation.

The idea failed. Instructions went up, not down: pinned `perf stat`
counted 5.44 billion before the change and 5.75 billion after, a
5.64% rise. Cycles rose 6.78%. Wall clock rose 7.7%. Peak RSS also
rose, instead of falling. Every axis moved the wrong way, with no
benefit to offset the cost.

The cause is the hash function itself. The old cache hashed the
source pointer, a fixed, length-independent cost. A content-keyed
cache must hash the string's own bytes instead, a cost that grows
with the string's length. It is paid on every lookup, including the
within-derivation hits that used to be nearly free.
`find_escape_aterm`'s own SWAR scan, the cost this cache exists to
skip, is already fast. Round 4 of this document found it near its
own ceiling. Hashing every byte to find a cache slot was not cheaper
than scanning those same bytes to find an escape. The trade lost on
this benchmark.

The change was fully reverted, not left in the tree. 44 of 44
difftest cases and the full nixpkgs-hash sweep passed both before
and after this attempt, so correctness was never in question here.
Only instructions, cycles, wall clock, and RSS, all measured with
pinned `perf stat` and `/usr/bin/time -v`, decided this. Do not
re-attempt a content-keyed cache for this exact cost, ATerm escaping,
without a real change in the tradeoff. A cheaper content hash, or a
workload with a much higher cross-derivation hit rate, changes that
balance.

## A missing port, found by comparing branch misses against asnix

A fresh `ll`-vs-`iets` comparison on `ffmpeg-full.outPath` used
pinned `perf stat`. It found `ll` with 31.7 million branch misses
against `iets`'s 19.5 million, a much larger relative gap than the
instruction-count gap alone. `asnix`, built from the same lineage
and using the same threaded-dispatch design as `ll`, had only 27.7
million. Two evaluators that use the same dispatch architecture do
not usually differ by that much on branch misses, from the
architecture alone. This pointed at something specific to `ll`, not
a shared cost.

`perf record -e branch-misses` on both binaries found the gap spread
across glibc's own allocator functions. `_int_malloc`, `cfree`, and
`malloc` together carried about 2.48 million branch misses in `ll`.
The equivalent allocator functions in `asnix` carried about 0.90
million. `asnix`'s own Makefile already links mimalloc, a faster
allocator, as a process-wide override, from an earlier milestone
(M146) that measured a real win there. `ll`'s own `flake.nix` already
exports the same `ASNIX_MIMALLOC_LIB` variable asnix's Makefile
consumes, but `ll`'s Makefile never read it. The milestone was never
ported, not a result of any measurement against `ll` itself.

The fix ports the same Makefile logic `asnix` already uses. If
`ASNIX_MIMALLOC_LIB` is set, the build links that static archive.
Otherwise, it probes for a system-installed mimalloc. Either way,
this needs no source change, because mimalloc's own archive already
exports standard `malloc`/`free`/`calloc`/`realloc` symbols.
Measured on `ffmpeg-full.outPath` (pinned `perf stat`, three runs
each):

- instructions: down 6.66%
- cycles: down 5.93%
- wall clock: down 6.4%
- branch misses: down 11.56%, from 31.5 million to 27.9 million,
  closing nearly all of the gap with `asnix`'s own 27.7 million
- peak RSS: up 2.4%, about 10 MB
- minor page faults: down 3.9%, despite the higher RSS

The RSS increase is a known mimalloc tradeoff (larger upfront
segment reservations), not a new regression this fix introduced.
Verified: 44/44 difftest, all 148 nixpkgs-hash pairs, a byte-
identical `ffmpeg-full.outPath` diff against `asnix`.
