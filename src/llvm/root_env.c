/* A root Env with the same 16-slot shape compiler.c's bare-global name
 * resolution hardcodes (see compiler.c's own comment: "The root env
 * (constructed by eval_file.c) has sixteen slots... in the same order as
 * eval_file.c's make_root_env_ex"), built from the SAME vendored PrimOp
 * constructors eval_file.c's own (static, so not callable from here)
 * make_root_env_ex uses -- just with the `derivation` slot filled by
 * asnix_make_builtins_attrs_for_derivation_bootstrap()'s PLAIN
 * `derivationStrict` wrapper instead of the fancy wrapper
 * asnix_make_builtins_attrs() would build.
 *
 * That fancy wrapper (derivation.c's asnix_make_derivation_wrapper_value)
 * bootstraps itself by compiling and EVALUATING an embedded .nix source
 * file (asnix_eval_file_for_derivation_bootstrap) -- real attrset/
 * string/OP_SELECT Nix source this translator doesn't support yet. Every
 * other slot here is a plain C-constructed PrimOp/Attrs heap value, no
 * compiled-and-evaluated Nix source involved, so none of them share that
 * problem. Revisit once attrsets/strings/select land (M6+) and switch
 * back to the real eval_file.c root env for full fidelity. */
#include "asnix/env.h"
#include "asnix/value.h"
#include "root_env.h"

#include "builtins.h"
#include "builtins_attrs.h"
#include "builtins_fetchgit.h"
#include "builtins_m54.h"
#include "builtins_misc.h"
#include "builtins_toml.h"
#include "derivation.h"

Env *ll_make_root_env(void) {
    Env *root_env = asnix_env_alloc(NULL, 16);
    root_env->values[0] = asnix_make_builtins_attrs_for_derivation_bootstrap();
    root_env->values[1] = asnix_make_import_primop();
    root_env->values[2] = asnix_make_map_primop();
    root_env->values[3] = asnix_make_abort_primop();
    root_env->values[4] = asnix_make_toString_primop();
    root_env->values[5] = asnix_make_removeAttrs_primop();
    root_env->values[6] = asnix_make_isNull_primop();
    root_env->values[7] = asnix_make_throw_primop();
    root_env->values[8] = asnix_make_derivation_strict_primop();
    root_env->values[9] = asnix_make_baseNameOf_primop();
    root_env->values[10] = asnix_make_dirOf_primop();
    root_env->values[11] = asnix_make_fromTOML_primop();
    root_env->values[12] = asnix_make_placeholder_primop();
    root_env->values[13] = asnix_make_scopedImport_primop();
    root_env->values[14] = asnix_make_derivation_strict_primop();
    root_env->values[15] = asnix_make_fetchGit_primop();
    return root_env;
}
