/* A root Env with the same 16-slot shape compiler.c's bare-global name
 * resolution hardcodes (see compiler.c's own comment: the root env has
 * sixteen slots, in the same order as eval_file.c's
 * make_root_env_ex), built from the SAME vendored PrimOp constructors
 * eval_file.c's own (static, so not callable from here)
 * make_root_env_ex(up, 0) (the real, non-bootstrap path) uses -- slot 0
 * is the real `ll_make_builtins_attrs()`, slot 8 is the real
 * `derivation` wrapper (ll_make_derivation_wrapper_value), matching
 * eval_file.c's ordinary root env exactly. */
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
