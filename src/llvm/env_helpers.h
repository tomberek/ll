#ifndef LL_ENV_HELPERS_H
#define LL_ENV_HELPERS_H

#include "ll/env.h"
#include "ll/value.h"

void ll_env_set(Env *e, int64_t slot, Value v);
Env *ll_env_frame_exit(Env *e);

#endif /* LL_ENV_HELPERS_H */
