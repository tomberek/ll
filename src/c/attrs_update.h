/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef ASNIX_ATTRS_UPDATE_H
#define ASNIX_ATTRS_UPDATE_H

#include "asnix/value.h"

/* `a // b`: right-biased merge of two Attrs heap objects (both already
 * type-checked ASNIX_TAG_ATTRS by op_update.S's caller). Entries are
 * sorted-by-symbol-id arrays (see heap.h), so this is a plain two-pointer
 * sorted merge, O(|a|+|b|); on a duplicate key, `b`'s entry (value and
 * all) is kept verbatim, matching real Nix's right-wins `//` semantics.
 * Neither input is mutated or forced -- values (including any still-
 * unforced Thunk) are copied through unchanged. */
Value asnix_attrs_update(Value lhs, Value rhs);

/* M88: the pure flat-array two-pointer merge `asnix_attrs_update` used to
 * do unconditionally -- now also reused by `asnix_bshared_attrs_resolve`
 * (builtins_shared.c) to materialize a lazy union header's leaves. BOTH
 * `lhs`/`rhs` MUST already be flat (non-negative `count`, heap.h) --
 * callers that aren't sure resolve first. */
Value asnix_attrs_eager_merge(Value lhs, Value rhs);

/* `${nameExpr} = value;` inside a plain `{...}`/`rec {...}` literal
 * (`OP_INSERT_DYNAMIC_ATTR`, ir.h): inserts one dynamically-named entry
 * into `attrs` (already fully built from the literal's static bindings),
 * keeping the sorted-by-symbol-id invariant. Fatal if `name_val` isn't a
 * string, or if the name collides with an entry already in `attrs`
 * (matching real Nix's own behavior: a plain attrset literal's duplicate
 * key -- static or dynamic -- is a hard error, "attribute ... already
 * defined", unlike `//`'s silent right-wins semantics above). `attrs`
 * itself is never mutated (same never-mutate convention as
 * asnix_attrs_update) -- a fresh, one-larger sorted array is built. */
Value asnix_attrs_insert_dynamic(Value attrs, Value name_val, Value value);

#endif /* ASNIX_ATTRS_UPDATE_H */
