/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_DERIVATION_H
#define LL_DERIVATION_H

#include <stddef.h>

#include "ll/value.h"
#include "builtins_misc.h" /* LlBuiltinMiscDef -- reused for LL_DERIVATION_BUILTINS below,
                             * same shared-shape precedent builtins_attrs.h already set for
                             * builtins_list.h's LlBuiltinDef. */

/*
 * `derivationStrict { ... }` (M8's minimal slice, see
 * docs/architecture.md; a later milestone generalized it to arbitrary
 * named outputs, see below): zero input derivations, zero input
 * sources. Required attrs: `name`, `system`, `builder`. Optional:
 * `args` (list of strings), `outputs` (list of strings, default
 * `["out"]`, same default the Nix-level `derivation` wrapper below
 * assumes). Every other attr becomes an environment variable (coerced
 * to a string via `ll_coerce_to_string`, i.e. the same rules as
 * `builtins.toString`) -- including `outputs` itself, which is ALSO
 * parsed into the actual set of declared output names (matching real
 * Nix: a multi-output `.drv`'s env genuinely has an `outputs = "out
 * bin ..."` string var, consumed by build-time setup hooks, alongside
 * the structured per-output info in the ATerm's own `Outputs` section).
 *
 * `__structuredAttrs = true;` (M67) switches the "every other attr
 * becomes an env var" rule above off: instead, every attr (except
 * `args`) is JSON-serialized (the exact `builtins.toJSON` encoder,
 * `ll_toJSON_cstr` in builtins_compare.c) into one `__json` env var,
 * which is what lets an attribute whose value isn't coercible to a
 * plain string (e.g. `env = { foo = "bar"; };`, a nested attrset) flow
 * through instead of hitting `ll_coerce_to_string`'s fatal path --
 * see derivation.c's `ll_prim_derivation` and docs/architecture.md's
 * M67 scope notes for the full algorithm (ported directly from
 * `~/nix/src/libexpr/primops.cc`'s `derivationStrictInternal`). The
 * masked/real two-pass output-path hashing below is unaffected either
 * way -- every declared output still gets its own env var, structured
 * mode or not.
 *
 * One distinct store path is computed per declared output name (the
 * masked/self-referential-hash two-pass scheme generalizes directly:
 * ONE masked hash covers every output's blanked entry at once, then
 * each output's own real path is `makeOutputPath(outputName,
 * maskedHash, outputPathName(name, outputName))` -- see derivation.c's
 * `output_path_name`/`atermoutput_cmp`, ported from real Nix's
 * `~/nix/src/libstore/derivations.cc`).
 *
 * Returns `{ drvPath; <outputName> = <that output's outPath>; ... }`
 * for every declared output -- exactly real Nix's own
 * `derivationStrict` shape (`~/nix/src/libexpr/primops.cc`'s
 * `mkOutputString`: a plain string per output, NOT a nested attrset,
 * and no separate top-level `outPath`/`type`/`out` keys). Does NOT
 * merge the original input attrs back into the result or build the
 * self-referential per-output "nice" object (`drv.out.outPath`, etc.)
 * -- that's exactly what real Nix's OWN `derivation` does on top of
 * this primop, via a Nix-level wrapper (`~/nix/src/libexpr/primops/
 * derivation.nix`) that this project ports verbatim (M58, see
 * ll_make_derivation_wrapper_value below) rather than reimplementing
 * in C. Does not track string context (so a derivation's outPath can't
 * yet be used as another derivation's input -- that needs the recursive
 * `hashDerivationModulo` masking real Nix does for input derivations,
 * out of scope here), and fixed-output (content-addressed) derivations
 * remain restricted to a single "out" output, matching real Nix's own
 * restriction there -- multi-output fixed-output/CA derivations are
 * out of scope (see derivation.c's header comment on
 * `validate_output_hash_fields` for the pre-existing fixed-output scope
 * cut this builds on).
 */
Value ll_prim_derivation(const Value *args, int64_t argc);

/* M80: real Nix's `makeFixedOutputPath` formula (M73's
 * `ll_compute_fixed_output_path`, ~/nix/src/libstore/
 * store-dir-config.cc) -- exposed publicly so `builtins.path`'s
 * `recursive=false`/pinned-`sha256` modes (builtins_path.c) can reuse
 * it exactly rather than re-deriving the same formula a second time.
 * `output_hash_mode` is `"recursive"` or `"flat"` (anything else is
 * treated as `"flat"`, same as the derivation-attribute caller this
 * was ported from); `algo`/`algo_len` name the hash algorithm (this
 * project's own callers only ever pass `"sha256"`); `raw`/`raw_len` is
 * that algorithm's raw digest bytes (a NAR hash for `"recursive"`, a
 * flat content hash otherwise). */
void ll_compute_fixed_output_path(const char *output_hash_mode, const uint8_t raw[64], size_t raw_len,
                                      const char *algo, size_t algo_len, const char *name, char *out,
                                      size_t out_cap);

/* The raw primop above, wrapped as a Value -- bound to the bare global
 * `derivationStrict` (and `builtins.derivationStrict`), exactly matching
 * real Nix's own split between the low-level primop and the `derivation`
 * wrapper below that's actually built on top of it. */
Value ll_make_derivation_strict_primop(void);

/* M58: real Nix's `derivation` is NOT a primop -- it's a short Nix-level
 * function (ported verbatim below from cppnix's own
 * `~/nix/src/libexpr/primops/derivation.nix`) that calls
 * `derivationStrict` and then wraps the result in a self-referential
 * "nice" object: `drv.out` is the WHOLE output's own attrset (so
 * `drv.out.outPath`/`drv.out.drvPath`/`drv.out.type` all work, not just
 * `drv.outPath`), and `drv.all`/`drv.drvAttrs`/the original input attrs
 * (`name`, `system`, `builder`, ...) are all merged back in too.
 * Verified directly against real Nix (`(derivation {...}).out` has
 * `builtins.typeOf == "set"`, not "string" -- asnix's previous
 * `out = outPath` string was simply wrong). Found via `d.nix`: nixpkgs'
 * `lib.extendDerivation` (used by every `mkDerivation`-built package,
 * and by minimal-bootstrap's own `derivationWithMeta`) does
 * `inherit (drv.${outputName}) type outputName;` and
 * `drv.${outputName}.outPath` -- both require `drv.out` to be attrset-
 * shaped, which crashed with "selection target is not an attrset" once
 * `out`'s own `.outPath` got selected a second time. Lazily compiled and
 * evaluated once per process (same write-a-fixed-temp-path-once pattern
 * compiler.c's `<nix/fetchurl.nix>` support already established, M46)
 * and the resulting Closure Value is cached -- it's a pure function with
 * no caller-specific state, so reusing one across every root Env's
 * `derivation` binding is behaviorally identical to rebuilding it fresh
 * each time, just without paying the lex/parse/compile cost repeatedly. */
Value ll_make_derivation_wrapper_value(void);

/* `builtins.derivation`/`builtins.derivationStrict` -- same two Values
 * as the bare globals above, wired into ll_make_builtins_attrs
 * (builtins.c) via the same {name,build()} integration step every other
 * builtins-expansion module uses. */
extern const LlBuiltinMiscDef LL_DERIVATION_BUILTINS[];
extern const size_t LL_DERIVATION_BUILTINS_COUNT;

#endif /* LL_DERIVATION_H */
