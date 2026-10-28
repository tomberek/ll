#ifndef ASNIX_ART_SYM_H
#define ASNIX_ART_SYM_H

#include <stddef.h>
#include <stdint.h>

/*
 * M195: specialized, mutable (non-persistent) ART-style radix trie keyed
 * on the 4 bytes of a uint32_t symbol id, used ONLY to replace
 * zipAttrsWith2_impl's (builtins_m54.c) "collect every (sym) across all
 * input attrsets, sort, dedupe" step. Not a general attrset
 * representation change -- asnix_bshared_make_attrs's flat sorted array
 * is still what every attrset actually IS; this type is a throwaway
 * scratch structure built once per zipAttrsWith call and read out via one
 * in-order traversal, same lifetime as the `all_syms`/`distinct` scratch
 * arrays it replaces.
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
 * nothing here actually needs (zipAttrsWith builds exactly one tree per
 * call and discards it immediately after one traversal -- no old root
 * ever has to stay valid). Fixing both (variable-size-per-kind node
 * structs; plain mutable in-place insertion via `ArtNode **ref`, same
 * pattern as this codebase's own asnix_arena_alloc bump arena) reversed
 * the result: callgrind instruction counts, duplicate-aware workload
 * (distinct_n ~60% of total_n, matching module-merge key overlap),
 * bulk-collect+qsort+dedupe baseline vs this ART:
 *   total_n=50:    ~199K   vs ~197K   (about even)
 *   total_n=200:   ~245K   vs ~227K   (ART wins, 1.08x)
 *   total_n=1000:  ~540K   vs ~364K   (ART wins, 1.48x)
 *   total_n=5000:  ~2.32M  vs ~1.04M  (ART wins, 2.23x)
 *   total_n=50000: ~26.56M vs ~10.19M (ART wins, 2.61x)
 * -- a real, growing win across the whole zipAttrsWith-relevant range
 * (tens to tens-of-thousands of total entries across many merged module
 * files), not just an asymptotic one. SIMD (SSE2 broadcast-compare-
 * movemask) was tried on Node4/Node16 key search in an even earlier
 * round and measured as a net LOSS there (fixed vector-setup overhead
 * exceeds a 4-16-entry early-exit scalar scan) -- not reused here; this
 * type's hot path is insert-heavy, not search-heavy, so it wouldn't have
 * applied anyway.
 *
 * All nodes are allocated via asnix_arena_alloc (never individually
 * freed, same as every other scratch allocation in this codebase) --
 * there is no separate arena/lifetime to manage here.
 */

typedef struct AsnixArtSymNode AsnixArtSymNode;

typedef struct {
    AsnixArtSymNode *root;
} AsnixArtSymSet;

static inline void asnix_art_sym_init(AsnixArtSymSet *set) { set->root = NULL; }

/* Inserts `sym` into the set. Idempotent: inserting an already-present
 * sym is a no-op (no allocation at all on that path). */
void asnix_art_sym_insert(AsnixArtSymSet *set, uint32_t sym);

/* Writes every distinct sym in the set, in ascending numeric order, into
 * `out` (caller-allocated; must hold at least as many entries as were
 * ever inserted, which is always >= the distinct count). Returns the
 * number of distinct syms written. */
int64_t asnix_art_sym_collect_sorted(const AsnixArtSymSet *set, uint32_t *out);

#endif /* ASNIX_ART_SYM_H */
