#ifndef LL_ART_SYM_H
#define LL_ART_SYM_H

#include <stddef.h>
#include <stdint.h>

#include "ll/value.h"

/*
 * Specialized, mutable (non-persistent) ART-style radix trie keyed on the
 * 4 bytes of a uint32_t symbol id, mapping sym -> Value. A duplicate
 * insert is a no-op (see ll_art_sym_insert below), giving "first
 * occurrence of a duplicate name wins" semantics for free from the
 * insert-then-traverse shape. Not a general attrset representation --
 * ll_bshared_make_attrs's flat sorted array is still what every attrset
 * actually IS; this type is a throwaway scratch structure built once per
 * call and read out via one in-order traversal.
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

/* Idempotent AND first-wins: if `sym` is already present, this is a
 * complete no-op (no allocation, and `val` is NOT written over the
 * existing entry), so the value stored for a given sym is always the one
 * from its FIRST insert call. */
void ll_art_sym_insert(LlArtSymSet *set, uint32_t sym, Value val);

/* Writes every distinct sym in the set, in ascending numeric order, into
 * `out_syms` (caller-allocated; must hold at least as many entries as
 * were ever inserted, which is always >= the distinct count). If
 * `out_vals` is non-NULL, also writes each sym's first-inserted Value at
 * the same output index (caller-allocated, same size requirement);
 * passing NULL skips writing values entirely. Returns the number of
 * distinct syms written. */
int64_t ll_art_sym_collect_sorted(const LlArtSymSet *set, uint32_t *out_syms, Value *out_vals);

#endif /* LL_ART_SYM_H */
