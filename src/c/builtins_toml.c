/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
/*
 * builtins.fromTOML doc -- arity 1 (a single string argument), no
 * currying needed. The real parsing work lives entirely in toml.c; this
 * file is just the builtins-table glue, same split derivation.c/
 * builtins.c already use for `derivation`.
 */
#include "builtins_toml.h"

#include <stdint.h>

#include "builtins_shared.h"
#include "toml.h"
#include "value_util.h"

static Value prim_fromTOML(const Value *args, int64_t argc) {
    (void)argc;
    const char *bytes;
    int64_t len;
    ll_bshared_string_bytes(args[0], &bytes, &len, "fromTOML");
    return ll_toml_parse(bytes, (size_t)len);
}

static Value build_fromTOML(void) { return ll_bshared_make_primop((void *)prim_fromTOML, 1); }

/* `fromTOML` is one of real Nix's bare root-scope globals -- confirmed
 * directly against `nix-instantiate --eval` (`fromTOML "a = 1"` works
 * bare), then cross-checked against cppnix's own source
 * (`src/libexpr/primops/fromTOML.cc`'s `RegisterPrimOp` has
 * `.name = "fromTOML"`, no `__` prefix -- `EvalState::addPrimOp`,
 * `eval.cc`, exposes every non-`__`-prefixed primop as a bare global
 * under that exact name, in addition to `builtins.<name>`; a `__`-
 * prefixed one, like `toJSON`'s real registered name `__toJSON`, is
 * NOT bare-accessible under its stripped name -- confirmed separately,
 * `toJSON {}` bare is "undefined variable"). This is the actual general
 * rule behind M18's "small set of bare globals" research, which had
 * been incomplete (missed `fromTOML`, found needed evaluating real
 * nixpkgs: `lib/trivial.nix` calls bare `fromTOML` directly). See
 * builtins.h's `ll_make_import_primop` comment for the full list as
 * currently understood; `fetchTarball`/`fetchGit`/`fetchMercurial`/
 * `fetchTree`/`scopedImport`/`placeholder`/`derivationStrict`/`break`
 * are also real (per the same source) -- at the time this comment was
 * written, none were implemented yet, and none were pursued
 * speculatively here (only `fromTOML` was actually hit); `scopedImport`/
 * `placeholder`/`derivationStrict` were added by M54/M58, and `fetchGit`
 * by M82 (builtins_fetchgit.c) once nixpkgs's own rust-toolchain
 * dependency chain actually referenced it -- `fetchTarball`/
 * `fetchMercurial`/`fetchTree`/`break` remain real gaps. */
Value ll_make_fromTOML_primop(void) { return ll_bshared_make_primop((void *)prim_fromTOML, 1); }

const LlBuiltinMiscDef LL_TOML_BUILTINS[] = {
    { "fromTOML", build_fromTOML },
};

const size_t LL_TOML_BUILTINS_COUNT = sizeof(LL_TOML_BUILTINS) / sizeof(LL_TOML_BUILTINS[0]);
