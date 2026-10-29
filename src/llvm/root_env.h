#ifndef LL_ROOT_ENV_H
#define LL_ROOT_ENV_H

#include "ll/env.h"

/* Builds the 16-slot root Env (builtins/map/import/etc.) -- see
 * root_env.c's header comment for why this isn't eval_file.c's own
 * make_root_env. */
Env *ll_make_root_env(void);

#endif /* LL_ROOT_ENV_H */
