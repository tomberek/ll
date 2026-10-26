#ifndef LL_RESOLVE_H
#define LL_RESOLVE_H
#include <stdint.h>

#include "asnix/heap.h"
#include "asnix/value.h"
#include "value_util.h"

extern void asnix_fatal(const char *msg, uint64_t len) __attribute__((noreturn));

/* Inlined, header-only on purpose: asnix's own op_var_ref.S/force.S once
 * called asnix_value_is_cell (which itself called asnix_tag_of) to probe
 * for a Cell, then switched to a direct tag-bit test after profiling
 * showed the two stacked real calls were measurable on every var-ref and
 * every force -- see those files' own comments. ll_resolve_indirection
 * hit the exact same cost (confirmed via callgrind on a real nixpkgs
 * eval: this function alone was 1.67% of total instructions, almost
 * entirely call/ret overhead around single tag-bit checks), so it gets
 * the same fix here: a `static inline` function every call site compiles
 * in directly, falling back to a real call only for the actual
 * (comparatively rare) LazyApp/ThunkMap resolution work.
 *
 * LL_PTR_BASE_MASK must match value_util.c's own ASNIX_PTR_BASE_MASK
 * exactly (same heap layout, same ThunkMap packed-index bits to clear --
 * see that file's own comment on the constant). Not shared via a header
 * because value_util.c is vendored byte-identical from asnix. */
#define LL_PTR_BASE_MASK 0x0000FFFFFFFFFFF8ULL

static inline Value ll_resolve_indirection(Value v) {
    int64_t cell_hops = 0;
    for (;;) {
        if ((v.raw & ASNIX_TAG_MASK) != ASNIX_TAG_PTR) return v;
        const char *base = (const char *)(uintptr_t)(v.raw & LL_PTR_BASE_MASK);
        int64_t marker = *(const int64_t *)base;

        if (marker == ASNIX_CELL_MARKER) {
            if (++cell_hops > ASNIX_CELL_CHASE_LIMIT) {
                static const char msg[] = "asnix: infinite recursion forcing a thunk (blackhole), aborting\n";
                asnix_fatal(msg, sizeof(msg) - 1);
            }
            v = **(Value **)(base + ASNIX_CELL_OFF_TARGET);
            continue;
        }
        if (marker == ASNIX_LAZYAPP_MARKER || marker == ASNIX_LAZYAPP_FORCED_MARKER) {
            v = asnix_resolve_lazyapp(v);
            continue;
        }
        if (marker == ASNIX_THUNKMAP_MARKER) {
            v = asnix_resolve_thunkmap(v);
            continue;
        }
        return v;
    }
}
#endif /* LL_RESOLVE_H */
