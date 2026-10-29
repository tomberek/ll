/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_DERIVATION_H
#define LL_DERIVATION_H

#include <stddef.h>

#include "ll/value.h"
#include "builtins_misc.h"

/*
 * `derivationStrict { ... }`: zero input derivations, zero input
 * sources. Required attrs: `name`, `system`, `builder`. Optional:
 * `args` (list of strings), `outputs` (list of strings, default
 * `["out"]`, same default the Nix-level `derivation` wrapper below
 * assumes). Every other attr becomes an environment variable (coerced
 * to a string via `ll_coerce_to_string`, i.e. the same rules as
 * `builtins.toString`) -- including `outputs` itself, which is ALSO
 * parsed into the actual set of declared output names.
 *
 * `__structuredAttrs = true;` switches the "every other attr becomes an
 * env var" rule above off: instead, every attr (except `args`) is
 * JSON-serialized into one `__json` env var, which is what lets an
 * attribute whose value isn't coercible to a plain string (e.g.
 * `env = { foo = "bar"; };`, a nested attrset) flow through instead of
 * hitting `ll_coerce_to_string`'s fatal path. The masked/real two-pass
 * output-path hashing below is unaffected either way.
 *
 * One distinct store path is computed per declared output name: ONE
 * masked hash covers every output's blanked entry at once, then each
 * output's own real path is `makeOutputPath(outputName, maskedHash,
 * outputPathName(name, outputName))`.
 *
 * Returns `{ drvPath; <outputName> = <that output's outPath>; ... }`
 * for every declared output -- a plain string per output, NOT a nested
 * attrset, and no separate top-level `outPath`/`type`/`out` keys. Does
 * NOT merge the original input attrs back into the result or build the
 * self-referential per-output "nice" object (`drv.out.outPath`, etc.)
 * -- that's what the `derivation` Nix-level wrapper below does on top
 * of this primop. Does not track string context (so a derivation's
 * outPath can't yet be used as another derivation's input), and
 * fixed-output (content-addressed) derivations remain restricted to a
 * single "out" output, matching real Nix's own restriction there.
 */
Value ll_prim_derivation(const Value *args, int64_t argc);

/* Real Nix's `makeFixedOutputPath` formula -- exposed publicly so
 * `builtins.path`'s `recursive=false`/pinned-`sha256` modes
 * (builtins_path.c) can reuse it exactly rather than re-deriving the
 * same formula a second time. `output_hash_mode` is `"recursive"` or
 * `"flat"` (anything else is treated as `"flat"`); `algo`/`algo_len`
 * name the hash algorithm (this project's own callers only ever pass
 * `"sha256"`); `raw`/`raw_len` is that algorithm's raw digest bytes (a
 * NAR hash for `"recursive"`, a flat content hash otherwise). */
void ll_compute_fixed_output_path(const char *output_hash_mode, const uint8_t raw[64], size_t raw_len,
                                      const char *algo, size_t algo_len, const char *name, char *out,
                                      size_t out_cap);

/* The raw primop above, wrapped as a Value -- bound to the bare global
 * `derivationStrict` (and `builtins.derivationStrict`), exactly matching
 * real Nix's own split between the low-level primop and the `derivation`
 * wrapper below that's actually built on top of it. */
Value ll_make_derivation_strict_primop(void);

/* Real Nix's `derivation` is NOT a primop -- it's a short Nix-level
 * function (ported verbatim below from cppnix's own
 * `~/nix/src/libexpr/primops/derivation.nix`) that calls
 * `derivationStrict` and then wraps the result in a self-referential
 * "nice" object: `drv.out` is the WHOLE output's own attrset (so
 * `drv.out.outPath`/`drv.out.drvPath`/`drv.out.type` all work, not just
 * `drv.outPath`), and `drv.all`/`drv.drvAttrs`/the original input attrs
 * (`name`, `system`, `builder`, ...) are all merged back in too. `drv.out`
 * MUST be attrset-shaped, not a plain outPath string -- nixpkgs'
 * `lib.extendDerivation` (used by every `mkDerivation`-built package)
 * does `inherit (drv.${outputName}) type outputName;` and
 * `drv.${outputName}.outPath`, which crashes with "selection target is
 * not an attrset" if `out` is a bare string. Lazily compiled and
 * evaluated once per process, and the resulting Closure
 * Value is cached -- it's a pure function with no caller-specific state,
 * so reusing one across every root Env's `derivation` binding is
 * behaviorally identical to rebuilding it fresh each time, just without
 * paying the lex/parse/compile cost repeatedly. */
Value ll_make_derivation_wrapper_value(void);

extern const LlBuiltinMiscDef LL_DERIVATION_BUILTINS[];
extern const size_t LL_DERIVATION_BUILTINS_COUNT;

#endif /* LL_DERIVATION_H */
