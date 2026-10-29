#ifndef LL_ART_SYM_H
#define LL_ART_SYM_H

#include <stddef.h>
#include <stdint.h>

#include "ll/value.h"

/*
 * M195: specialized, mutable (non-persistent) ART-style radix trie keyed
 * on the 4 bytes of a uint32_t symbol id. Originally built ONLY to
 * replace zipAttrsWith2_impl's (builtins_m54.c) "collect every (sym)
 * across all input attrsets, sort, dedupe" step; M198 generalized it into
 * a real sym->Value map (each leaf now carries a `Value`, written only
 * the FIRST time a given sym is inserted -- a duplicate insert is a no-op
 * that touches nothing, see ll_art_sym_insert's own comment) so
 * listToAttrs's "first occurrence of a duplicate name wins" dedupe (M196)
 * could be done by the SAME insert-then-traverse shape instead of its own
 * separate sort+dedupe pass -- the two problems are identical once you
 * notice "keep the sym's first-ever-inserted value" is exactly what this
 * structure already does by design. Not a general attrset representation
 * change -- ll_bshared_make_attrs's flat sorted array is still what
 * every attrset actually IS; this type is a throwaway scratch structure
 * built once per call and read out via one in-order traversal.
 *
 * Why ART here and not elsewhere: an earlier round benchmarked a generic,
 * textbook persistent (copy-on-write) ART against the flat array for
 * general attrset lookup/incremental-build and found no unconditional
 * win -- flat arrays win decisively below ~16 entries, and a naive
 * generic ART only started winning in the high tens/hundreds. That
 * generic prototype ALSO lost to zipAttrsWith's own already-optimized
 * bulk collect+qsort+dedupe baseline at every size tested, because it
 * paid two unnecessary costs: (1) every node was one fixed-size struct
 * sized for the largest node kind (~2056 bytes, dominated by a 256-
 * pointer Node256 union member) even for a 2-entry Node4 or a bare leaf,
 * and (2) copy-on-write cloning on every insert, a persistence guarantee
 * nothing here actually needs (every call site builds exactly one tree
 * and discards it immediately after one traversal -- no old root ever
 * has to stay valid). Fixing both (variable-size-per-kind node structs;
 * plain mutable in-place insertion via `ArtNode **ref`, same pattern as
 * this codebase's own ll_arena_alloc bump arena) reversed the result:
 * callgrind instruction counts, duplicate-aware workload (distinct_n
 * ~60% of total_n, matching module-merge key overlap), bulk-collect+
 * qsort+dedupe baseline vs this ART:
 *   total_n=50:    ~199K   vs ~197K   (about even)
 *   total_n=200:   ~245K   vs ~227K   (ART wins, 1.08x)
 *   total_n=1000:  ~540K   vs ~364K   (ART wins, 1.48x)
 *   total_n=5000:  ~2.32M  vs ~1.04M  (ART wins, 2.23x)
 *   total_n=50000: ~26.56M vs ~10.19M (ART wins, 2.61x)
 * -- a real, growing win across the whole zipAttrsWith-relevant range,
 * not just an asymptotic one. SIMD (SSE2 broadcast-compare-movemask) was
 * tried on Node4/Node16 key search in an even earlier round and measured
 * as a net LOSS there (fixed vector-setup overhead exceeds a 4-16-entry
 * early-exit scalar scan) -- not reused here; every known use of this
 * type is insert-heavy, not search-heavy, so it wouldn't apply anyway.
 *
 * All nodes are allocated via ll_arena_alloc (never individually
 * freed, same as every other scratch allocation in this codebase) --
 * there is no separate arena/lifetime to manage here.
 */

typedef struct LlArtSymNode LlArtSymNode;

typedef struct {
    LlArtSymNode *root;
} LlArtSymSet;

static inline void ll_art_sym_init(LlArtSymSet *set) { set->root = NULL; }

/* Inserts `sym` -> `val` into the set. Idempotent AND first-wins: if
 * `sym` is already present, this is a complete no-op (no allocation, and
 * -- critically -- `val` is NOT written over the existing entry), so the
 * value stored for a given sym is always the one from its FIRST insert
 * call. This is exactly real Nix's documented listToAttrs/zipAttrsWith-
 * adjacent "first occurrence of a duplicate name wins" rule; callers that
 * don't need a payload (e.g. zipAttrsWith's own distinct-syms-only use)
 * can just pass `(Value){0}` and ignore `out_vals` in the collect call
 * below. */
void ll_art_sym_insert(LlArtSymSet *set, uint32_t sym, Value val);

/* Writes every distinct sym in the set, in ascending numeric order, into
 * `out_syms` (caller-allocated; must hold at least as many entries as
 * were ever inserted, which is always >= the distinct count). If
 * `out_vals` is non-NULL, also writes each sym's first-inserted Value at
 * the same output index (caller-allocated, same size requirement);
 * passing NULL skips writing values entirely (zero extra cost) for
 * callers that only need the sorted distinct sym list. Returns the
 * number of distinct syms written. */
int64_t ll_art_sym_collect_sorted(const LlArtSymSet *set, uint32_t *out_syms, Value *out_vals);

#endif /* LL_ART_SYM_H */
