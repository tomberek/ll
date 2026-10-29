/* Tiny glue around the vendored Env representation (include/ll/env.h,
 * src/asm/env.S) for the two operations op_frame.S inlines in asm that
 * don't already have a C-callable equivalent: writing a frame slot
 * (op_frame_set.S) and restoring the enclosing Env on scope exit
 * (op_frame_exit.S). Kept as real C functions, called from JIT'd code via
 * extern "C", rather than hand-rolled GEP/pointer arithmetic in
 * irop_to_llvm.c -- same reasoning as calling ll_add_values etc. for
 * arithmetic's slow path: struct-layout-sensitive logic belongs next to
 * the struct definition, not duplicated in the IR translator. */
#include <stdint.h>

#include "ll/env.h"
#include "ll/value.h"

void ll_env_set(Env *e, int64_t slot, Value v) {
    e->values[slot] = v;
}

/* Matches op_frame_exit.S exactly: no LL_ENV_WITH_TAG masking --
 * FRAME_EXIT only ever closes a frame FRAME_ALLOC opened (let/rec/lambda
 * call), never a with-frame (that's OP_WITH_EXIT, a different op), and an
 * ordinary frame's `up` never has the tag bit set. */
Env *ll_env_frame_exit(Env *e) {
    return (Env *)((uint8_t *)ll_env_arena_base + e->up);
}
