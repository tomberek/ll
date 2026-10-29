/* A root Env with the same 16-slot shape compiler.c's bare-global name
 * resolution hardcodes (see compiler.c's own comment: "The root env
 * (constructed by eval_file.c) has sixteen slots... in the same order as
 * eval_file.c's make_root_env_ex"), built from the SAME vendored PrimOp
 * constructors eval_file.c's own (static, so not callable from here)
 * make_root_env_ex(up, 0) (the real, non-bootstrap path) uses.
 *
 * M7 conformance fix: this used to always fill slot 0/8 with the
 * bootstrap-only `skip_derivation_wrapper=1` variants
 * (ll_make_builtins_attrs_for_derivation_bootstrap() and
 * ll_make_derivation_strict_primop(), plain `derivationStrict`
 * standing in for both `builtins.derivation` and bare `derivation`),
 * left over from before M6 when attrsets/strings/OP_SELECT weren't
 * compilable yet, so the real Nix-level `derivation` wrapper
 * (derivation.c's ll_make_derivation_wrapper_value, which
 * compiles+evaluates an embedded .nix source) couldn't be bootstrapped
 * for the TOP-LEVEL program's own root env. That gap was invisible to
 * the nixpkgs-hash sweep because `import`-ed files get their root env
 * from eval_file.c's own make_root_env_ex(up, 0) (already correct,
 * skip=0) -- only a top-level file's own bare `derivation {...}` (e.g.
 * nix-conformance's eval-okay-context-builtins.nix) actually went
 * through this file's skip=1 path, silently getting a result with no
 * `outPath`/`drvPath`/`type`/`outputName`/`outputs`/`all`/etc. attrs
 * at all (just derivationStrict's raw per-output path strings) --
 * coercing that bare attrset to a string later fails with "cannot
 * coerce a set to a string" since `ll_coerce_to_string`'s own
 * ATTRS case looks for exactly those attrs and finds none. The stale
 * comment that used to live here ("real attrset/string/OP_SELECT Nix
 * source this translator doesn't support yet... revisit once
 * attrsets/strings/select land (M6+)") described a pre-M6 limitation
 * that no longer applied by the time this bug was found -- M6 landed
 * attrsets/select, so the fancy wrapper evaluates fine; this file's
 * own slots just never got updated to use it. Now matches
 * eval_file.c's ordinary (non-bootstrap) root env exactly: real
 * `ll_make_builtins_attrs()` for slot 0, the real wrapper
 * (ll_make_derivation_wrapper_value) for slot 8. Every other slot
 * here is a plain C-constructed PrimOp/Attrs heap value, no compiled-
 * and-evaluated Nix source involved, so none of them ever had this
 * problem. */
#include "ll/env.h"
#include "ll/value.h"
#include "root_env.h"

#include "builtins.h"
#include "builtins_attrs.h"
#include "builtins_fetchgit.h"
#include "builtins_m54.h"
#include "builtins_misc.h"
#include "builtins_toml.h"
#include "derivation.h"

Env *ll_make_root_env(void) {
    Env *root_env = ll_env_alloc(NULL, 16);
    root_env->values[0] = ll_make_builtins_attrs();
    root_env->values[1] = ll_make_import_primop();
    root_env->values[2] = ll_make_map_primop();
    root_env->values[3] = ll_make_abort_primop();
    root_env->values[4] = ll_make_toString_primop();
    root_env->values[5] = ll_make_removeAttrs_primop();
    root_env->values[6] = ll_make_isNull_primop();
    root_env->values[7] = ll_make_throw_primop();
    root_env->values[8] = ll_make_derivation_wrapper_value();
    root_env->values[9] = ll_make_baseNameOf_primop();
    root_env->values[10] = ll_make_dirOf_primop();
    root_env->values[11] = ll_make_fromTOML_primop();
    root_env->values[12] = ll_make_placeholder_primop();
    root_env->values[13] = ll_make_scopedImport_primop();
    root_env->values[14] = ll_make_derivation_strict_primop();
    root_env->values[15] = ll_make_fetchGit_primop();
    return root_env;
}
