/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_BUILTINS_H
#define LL_BUILTINS_H

#include "ll/value.h"

/* Builds the `builtins` attrset (sorted by symbol id, matching OP_SELECT's
 * expectations) containing every native primop this milestone ships. The
 * compiler binds this as the single root-level scope slot (see
 * compiler.c/main.c) so user programs reference it like any other
 * variable: `builtins.toString 5`. */
Value ll_make_builtins_attrs(void);

/* M58: same as ll_make_builtins_attrs, but substitutes the plain
 * `derivationStrict` primop for the "derivation" entry too -- used only
 * by eval_file.c's ll_eval_file_for_derivation_bootstrap to break the
 * self-referential cycle building the `derivation` wrapper's own
 * `builtins` attrset would otherwise hit. See derivation.h. */
Value ll_make_builtins_attrs_for_derivation_bootstrap(void);

/* `import` is one of real Nix's bare-global names, exposed in addition
 * to `builtins.import`. The actual rule (read directly from cppnix's
 * source, `EvalState::addPrimOp`, `~/nix/src/libexpr/eval.cc`, after
 * M18's original piecemeal-testing-against-nix-instantiate approach
 * turned out to have missed names -- see M27's scope notes,
 * docs/architecture.md): *every* primop registered without a `__`
 * prefix is automatically exposed bare under that exact name, not a
 * small hand-picked list. Grepping every `RegisterPrimOp`/`.name = ...`
 * site for a non-`__`-prefixed name gives the complete set: `import
 * abort throw map removeAttrs isNull toString baseNameOf dirOf
 * fetchTarball fetchGit fetchMercurial fetchTree fetchFinalTree fromTOML
 * placeholder scopedImport derivationStrict break` -- plus `derivation`/
 * `builtins`/`true`/`false`/`null`/`__curPos`, which aren't ordinary
 * `addPrimOp`-registered primops and are handled separately. asnix
 * currently exposes `import`/`map`/`abort`/`throw`/`removeAttrs`/
 * `isNull`/`toString`/`derivation`/`baseNameOf`/`dirOf`/`fromTOML`/
 * `placeholder`/`scopedImport`/`derivationStrict`/`fetchGit` (M82, see
 * builtins_fetchgit.c) bare (see eval_file.c's root Env); the rest of
 * the fetch family (`fetchTarball`/`fetchMercurial`/`fetchTree`/
 * `fetchFinalTree`) and `break` (not yet hit by anything evaluated)
 * remain documented, deliberate follow-ups, not attempted speculatively.
 * Returns a fresh PrimOp Value wrapping the exact same native function
 * `builtins.import` uses -- same behavior, just reachable two ways. */
Value ll_make_import_primop(void);

/* Same idea, for `abort` -- also one of real Nix's bare-global names. */
Value ll_make_abort_primop(void);

/* Same idea, for `throw` -- also one of real Nix's bare-global names. */
Value ll_make_throw_primop(void);

/* Same idea, for `toString` -- also one of real Nix's bare-global names. */
Value ll_make_toString_primop(void);

/* M58: `derivation`/`derivationStrict`'s bare-global constructors moved
 * to derivation.h (ll_make_derivation_wrapper_value /
 * ll_make_derivation_strict_primop) -- `derivation` is no longer a
 * plain primop, see that header's comment. */

#endif /* LL_BUILTINS_H */
