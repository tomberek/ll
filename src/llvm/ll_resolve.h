#ifndef LL_RESOLVE_H
#define LL_RESOLVE_H
#include <stdint.h>

#include "ll/heap.h"
#include "ll/value.h"
#include "value_util.h"

extern void ll_fatal(const char *msg, uint64_t len) __attribute__((noreturn));

/* Inlined, header-only on purpose: asnix's own op_var_ref.S/force.S once
 * called ll_value_is_cell (which itself called ll_tag_of) to probe
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
 * LL_PTR_BASE_MASK must match value_util.c's own LL_PTR_BASE_MASK
 * exactly (same heap layout, same ThunkMap packed-index bits to clear --
 * see that file's own comment on the constant). Not shared via a header
 * because value_util.c is vendored byte-identical from asnix. */
#define LL_PTR_BASE_MASK 0x0000FFFFFFFFFFF8ULL

static inline Value ll_resolve_indirection(Value v) {
    int64_t cell_hops = 0;
    for (;;) {
        if ((v.raw & LL_TAG_MASK) != LL_TAG_PTR) return v;
        const char *base = (const char *)(uintptr_t)(v.raw & LL_PTR_BASE_MASK);
        int64_t marker = *(const int64_t *)base;

        if (marker == LL_CELL_MARKER) {
            if (++cell_hops > LL_CELL_CHASE_LIMIT) {
                static const char msg[] = "ll: infinite recursion forcing a thunk (blackhole), aborting\n";
                ll_fatal(msg, sizeof(msg) - 1);
            }
            v = **(Value **)(base + LL_CELL_OFF_TARGET);
            continue;
        }
        if (marker == LL_LAZYAPP_MARKER || marker == LL_LAZYAPP_FORCED_MARKER) {
            v = ll_resolve_lazyapp(v);
            continue;
        }
        if (marker == LL_THUNKMAP_MARKER) {
            v = ll_resolve_thunkmap(v);
            continue;
        }
        if (marker == LL_FORMALSMAP_MARKER) { /* ll-only, see heap.h */
            v = ll_resolve_formalsmap(v);
            continue;
        }
        return v;
    }
}
#endif /* LL_RESOLVE_H */
