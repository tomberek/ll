#ifndef LL_RESOLVE_H
#define LL_RESOLVE_H

#include "asnix/value.h"

/* Fully resolves Cell/LazyApp/ThunkMap indirection (heap.h's own
 * ASNIX_CELL_MARKER/ASNIX_LAZYAPP_MARKER/ASNIX_THUNKMAP_MARKER
 * comments), looping until none apply. Does NOT force a Thunk --
 * callers check for that tag themselves, since Thunk-forcing differs
 * by context (inline dispatch re-entry vs. the out-of-loop sentinel/
 * epilogue trick). Uses the vendored, out-of-line reference
 * implementations (value_util.c's asnix_resolve_cell_chain/
 * asnix_resolve_lazyapp/asnix_resolve_thunkmap) rather than asnix's
 * own hand-inlined hot-path copies of the same loops -- correct
 * first, same precedent this backend's OP_CALL already set by not
 * inlining the closure-call fast path through asnix_apply. */
Value ll_resolve_indirection(Value v);

#endif /* LL_RESOLVE_H */
