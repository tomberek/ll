#include <stdint.h>

#include "ll/env.h"
#include "ll/value.h"

void ll_env_set(Env *e, int64_t slot, Value v) {
    e->values[slot] = v;
}

/* No LL_ENV_WITH_TAG masking -- FRAME_EXIT only ever closes a frame
 * FRAME_ALLOC opened (let/rec/lambda call), never a with-frame (that's
 * OP_WITH_EXIT, a different op), and an ordinary frame's `up` never
 * has the tag bit set. */
Env *ll_env_frame_exit(Env *e) {
    return (Env *)((uint8_t *)ll_env_arena_base + e->up);
}
