/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "attrs_update.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/heap.h"
#include "asnix/stats.h" /* M112: visibility into this call site's real byte cost */
#include "builtins_shared.h" /* asnix_bshared_type_name, see M50 */
#include "string_context.h" /* M80: dynamic attr-name context rejection */
#include "symtab.h"
#include "value_util.h"

/* M88 threshold constants for lazy union-layering -- see heap.h's UnionHdr
 * comment and docs/architecture.md's M88 section for the full writeup.
 *
 * ASNIX_UNION_RHS_MAX: the RHS of `//` only gets deferred into a leaf (never
 * copied) when it's small. Chosen to match real Nix's own `Bindings`
 * layering threshold (~/nix/gc-unlock/src/libexpr/attr-set.hh's
 * `layerOnTopOf`, default 16) -- real nixpkgs overlay/override RHS attrsets
 * (a `postInstall` override, a `meta` patch, an overlay's `self: super: {
 * foo = ...; }`) are typically single-digit-to-low-double-digit in key
 * count, so 16 comfortably covers the common case without deferring
 * something big enough that the eventual materialize cost dominates.
 *
 * ASNIX_UNION_LHS_MIN: the LHS must be at least this big for skipping the
 * copy to be worth the extra indirection (a flat_cache check + a handful of
 * leaf-array words) -- below this, the eager two-pointer merge is already
 * a handful of memcpys and not worth avoiding. Set equal to RHS_MAX for
 * the same "matches real Nix's one documented threshold" reasoning; a real
 * stdenv.mkDerivation attrset (d.nix's ffmpeg closure) has 30-60+ top-level
 * keys by the time `derivation {...}` sees it, comfortably above this.
 *
 * ASNIX_ATTRS_UNION_MAX_LEAVES (heap.h): bounds worst-case leaf-chain depth
 * (and thus worst-case one-time materialize cost) the same way real Nix's
 * own layer-depth cap bounds worst-case `Bindings::get` cost -- once a
 * union header already has this many leaves, the NEXT `//` forces a
 * materialize-and-restart instead of growing the chain further.
 *
 * Empirically tuned, not just guessed: a first pass at 8 (iets's own
 * MAX_LEAVES) measured WORSE than always-eager on a synthetic
 * "foldl' chaining N small `//`s onto one big base, read once at the end"
 * benchmark (peak RSS ~identical to eager, see docs/architecture.md's M88
 * section) -- forcing a restart every 8 leaves means the amortized cost
 * per `//` is still O(total_size / 8), same ORDER as eager, just divided
 * by a small constant that per-call bookkeeping overhead ate entirely.
 * 64 leaves (after also fixing asnix_bshared_attrs_resolve's materialize
 * step to merge pairwise/bottom-up instead of left-to-right, see that
 * function's own comment) measured an 8.8x peak-RSS reduction and a
 * modest instruction-count win on the same benchmark, with no measured
 * regression on the real d.nix (ffmpeg) corpus -- see M88 for the full
 * numbers. Kept well under 128 (measured worse again at that size on the
 * same benchmark, likely cache-locality-related: a 128-leaf header's own
 * leaf array no longer fits as comfortably, and bottom-up-merge recursion
 * depth grows) without chasing a perfectly-optimal constant further.
 */
#define ASNIX_UNION_RHS_MAX 16
#define ASNIX_UNION_LHS_MIN 16

Value asnix_attrs_eager_merge(Value lhs, Value rhs) {
    const char *abase = (const char *)(lhs.raw & ~0x7ULL);
    const char *bbase = (const char *)(rhs.raw & ~0x7ULL);
    int64_t acount = *(const int64_t *)(abase + ASNIX_ATTRS_OFF_COUNT);
    int64_t bcount = *(const int64_t *)(bbase + ASNIX_ATTRS_OFF_COUNT);
    const char *aentries = abase + ASNIX_ATTRS_OFF_ENTRIES;
    const char *bentries = bbase + ASNIX_ATTRS_OFF_ENTRIES;

    /* Upper bound on the merged count (shrinks below acount+bcount only if
     * there are duplicate keys) -- allocate once for the worst case and
     * just record however many entries actually land, same "don't bother
     * reclaiming the slack" tradeoff the never-free arena already makes
     * everywhere else. */
    void *mem = asnix_arena_alloc(8 + (uint64_t)(acount + bcount) * ASNIX_ATTR_SIZE);
    char *out_entries = (char *)mem + ASNIX_ATTRS_OFF_ENTRIES;

    /* M143: walk with running byte pointers instead of `i`/`j`/`k` indices
     * recomputed via `idx * ASNIX_ATTR_SIZE` every iteration. The index
     * form looked equivalent to the compiler but wasn't: GCC couldn't
     * prove it could hoist the index->address multiply out of the loop
     * (ASNIX_ATTR_SIZE=12 isn't a shift), so the main merge loop's -O2
     * codegen recomputed `base + idx*12` from scratch (two extra `lea`s
     * per side, confirmed via `objdump -d` before this change) on EVERY
     * iteration, while the tail loops below (which this file already
     * wrote with a plain incrementing index multiplied once per access)
     * got the same treatment. Advancing plain `const char *` pointers by
     * a constant 12 each step needs no per-iteration multiply at all --
     * same two-pointer merge, same memcpy-per-entry, fewer retired
     * instructions in the loop iets's own `update_attrs` (see
     * docs/architecture.md's M143) also has to run. */
    const char *pa = aentries;
    const char *pb = bentries;
    const char *aend = aentries + acount * ASNIX_ATTR_SIZE;
    const char *bend = bentries + bcount * ASNIX_ATTR_SIZE;
    char *pout = out_entries;
    while (pa < aend && pb < bend) {
        uint32_t asym = *(const uint32_t *)(pa + ASNIX_ATTR_OFF_SYM);
        uint32_t bsym = *(const uint32_t *)(pb + ASNIX_ATTR_OFF_SYM);
        if (asym < bsym) {
            memcpy(pout, pa, ASNIX_ATTR_SIZE);
            pa += ASNIX_ATTR_SIZE;
        } else if (bsym < asym) {
            memcpy(pout, pb, ASNIX_ATTR_SIZE);
            pb += ASNIX_ATTR_SIZE;
        } else {
            /* duplicate key: rhs (b) wins, matching `//`'s right-biased semantics */
            memcpy(pout, pb, ASNIX_ATTR_SIZE);
            pa += ASNIX_ATTR_SIZE;
            pb += ASNIX_ATTR_SIZE;
        }
        pout += ASNIX_ATTR_SIZE;
    }
    while (pa < aend) {
        memcpy(pout, pa, ASNIX_ATTR_SIZE);
        pa += ASNIX_ATTR_SIZE;
        pout += ASNIX_ATTR_SIZE;
    }
    while (pb < bend) {
        memcpy(pout, pb, ASNIX_ATTR_SIZE);
        pb += ASNIX_ATTR_SIZE;
        pout += ASNIX_ATTR_SIZE;
    }

    int64_t k = (pout - out_entries) / ASNIX_ATTR_SIZE;
    *(int64_t *)((char *)mem + ASNIX_ATTRS_OFF_COUNT) = k;
    asnix_stats_attrs++;
    asnix_stats_attrs_entries += (uint64_t)k;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_ATTRS };
}

/* M88: builds a fresh UnionHdr (heap.h) whose leaves are `base_leaves`
 * (`n_base_leaves` of them, already flat, lowest-priority first) plus
 * `new_leaf` (also flat) on top. `n_base_leaves + 1` must be <=
 * ASNIX_ATTRS_UNION_MAX_LEAVES -- callers check that before calling this. */
static Value make_union_header(const Value *base_leaves, int64_t n_base_leaves, Value new_leaf) {
    int64_t nleaves = n_base_leaves + 1;
    void *mem = asnix_arena_alloc(ASNIX_ATTRS_UNION_OFF_LEAVES + (uint64_t)nleaves * sizeof(Value));
    *(int64_t *)((char *)mem + ASNIX_ATTRS_UNION_OFF_NLEAVES) = -nleaves;
    *(Value *)((char *)mem + ASNIX_ATTRS_UNION_OFF_FLATCACHE) = (Value){ .raw = 0 }; /* not yet materialized */
    Value *out_leaves = (Value *)((char *)mem + ASNIX_ATTRS_UNION_OFF_LEAVES);
    if (n_base_leaves > 0) memcpy(out_leaves, base_leaves, (size_t)n_base_leaves * sizeof(Value));
    out_leaves[n_base_leaves] = new_leaf;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_ATTRS };
}

/* `a // b`: M88 adds a lazy fast path on top of the always-correct eager
 * merge above -- when `rhs` is small and `lhs` is big (see the threshold
 * comment above ASNIX_UNION_RHS_MAX), this builds an O(1) UnionHdr
 * (heap.h) instead of copying the whole of `lhs`. Falls back to the
 * eager merge whenever the fast path doesn't apply, which remains fully
 * correct and is always a safe fallback. */
Value asnix_attrs_update(Value lhs, Value rhs) {
    /* rhs must be flat before it can become one leaf -- resolving an
     * already-flat Value is a single sign-bit check (see
     * asnix_bshared_attrs_resolve, builtins_shared.c), so this costs
     * nothing on the common case where rhs was never itself a union
     * (an ordinary `{ ... }` literal or any non-`//` expression). */
    Value rhs_flat = asnix_bshared_attrs_resolve(rhs);
    int64_t rhs_count = *(const int64_t *)((rhs_flat.raw & ~0x7ULL) + ASNIX_ATTRS_OFF_COUNT);

    const char *lhs_base = (const char *)(lhs.raw & ~0x7ULL);
    int64_t lhs_raw = *(const int64_t *)(lhs_base + ASNIX_ATTRS_OFF_COUNT);
    int lhs_is_union = lhs_raw < 0;
    int64_t lhs_leaves = lhs_is_union ? -lhs_raw : 1;

    if (rhs_count <= ASNIX_UNION_RHS_MAX && lhs_leaves < ASNIX_ATTRS_UNION_MAX_LEAVES
        && (lhs_is_union || lhs_raw >= ASNIX_UNION_LHS_MIN)) {
        if (lhs_is_union) {
            const Value *existing_leaves = (const Value *)(lhs_base + ASNIX_ATTRS_UNION_OFF_LEAVES);
            return make_union_header(existing_leaves, lhs_leaves, rhs_flat);
        }
        return make_union_header(&lhs, 1, rhs_flat);
    }

    /* Fallback: both operands must be flat for the two-pointer merge --
     * resolving lhs is the only one that can actually be a union here
     * (rhs_flat is already resolved above). */
    Value lhs_flat = asnix_bshared_attrs_resolve(lhs);
    return asnix_attrs_eager_merge(lhs_flat, rhs_flat);
}

Value asnix_attrs_insert_dynamic(Value attrs, Value name_val, Value value) {
    /* M88: `attrs` is always the direct output of a prior OP_MK_ATTRS or a
     * prior insert_dynamic call within the SAME `{...}`/`rec {...}`
     * literal (compiler.c's AST_ATTRS case) -- a `//` never runs in
     * between, so this is never actually a union header in practice.
     * Resolved anyway (a no-op sign-bit check when already flat) so this
     * holds even if that invariant ever changes, rather than relying on
     * an unchecked assumption. */
    attrs = asnix_bshared_attrs_resolve(attrs);
    /* `${null} = value;` is silently omitted, not an error -- a real,
     * deliberate nixpkgs idiom for a conditionally-present key
     * (`${if cond then "key" else null} = value;`), confirmed against
     * both real `nix-instantiate --eval` (`{ ${null} = 1; }` == `{}`,
     * while `{ ${1} = 1; }` IS a hard error) and cppnix's own source
     * (`~/nix/src/libexpr/eval.cc`'s attrset-construction loop: `if
     * (nameVal.type() == nNull) continue;`, checked *before* the
     * string-force/type-check below). Found via d.nix. */
    if (asnix_tag_of(name_val) == ASNIX_TAG_IMM && asnix_imm_subtag(name_val) == ASNIX_IMM_NULL) {
        return attrs;
    }
    if (asnix_tag_of(name_val) != ASNIX_TAG_PTR || asnix_value_is_path(name_val) || asnix_value_is_float(name_val)) {
        fprintf(stderr, "asnix: a dynamic '${...}' attribute name must be a string (got %s)\n",
                asnix_bshared_type_name(name_val));
        exit(1);
    }
    /* M80: a `${...}` attribute name cannot carry string context -- real
     * Nix's own `ExprAttrs::eval` calls `state.forceStringNoCtx` on a
     * dynamic attribute name (~/nix/src/libexpr/eval.cc), which hard-
     * errors ("the string '...' is not allowed to refer to a store path")
     * the moment the name's context is non-empty, confirmed directly:
     * `{ ${"${derivation {...}}"} = 1; }` fails on real
     * `nix-instantiate --eval` even though the same string, used as an
     * ordinary VALUE, is perfectly fine. Only this one call site is
     * patched (not e.g. `listToAttrs`) -- see this project's own
     * standing scope-cut convention of fixing exactly the call site a
     * failing fixture needs, not every symbol-from-string site at once. */
    const AsnixStringContext *name_ctx = asnix_string_context_of(name_val);
    if (!asnix_ctx_is_empty(name_ctx)) {
        fprintf(stderr, "asnix: the string used as a dynamic '${...}' attribute name is not allowed to refer to a "
                        "store path\n");
        exit(1);
    }
    int64_t name_len;
    const char *name_bytes = asnix_ptr_bytes(name_val, &name_len);
    uint32_t sym = asnix_intern(name_bytes, (size_t)name_len);

    const char *abase = (const char *)(attrs.raw & ~0x7ULL);
    int64_t acount = *(const int64_t *)(abase + ASNIX_ATTRS_OFF_COUNT);
    const char *aentries = abase + ASNIX_ATTRS_OFF_ENTRIES;

    int64_t lo = 0, hi = acount;
    while (lo < hi) {
        int64_t mid = lo + (hi - lo) / 2;
        uint32_t s = *(const uint32_t *)(aentries + mid * ASNIX_ATTR_SIZE + ASNIX_ATTR_OFF_SYM);
        if (s == sym) {
            fprintf(stderr, "asnix: dynamic attribute '%.*s' already defined, aborting\n", (int)name_len,
                    name_bytes);
            exit(1);
        }
        if (s < sym) lo = mid + 1; else hi = mid;
    }
    int64_t pos = lo; /* insertion point keeping sorted-by-sym order */

    void *mem = asnix_arena_alloc(8 + (uint64_t)(acount + 1) * ASNIX_ATTR_SIZE);
    char *out_entries = (char *)mem + ASNIX_ATTRS_OFF_ENTRIES;
    if (pos > 0) memcpy(out_entries, aentries, (size_t)pos * ASNIX_ATTR_SIZE);
    /* M103: must be a 4-byte write -- entry's value bytes now start right after sym (no pad),
     * so an 8-byte write here would clobber them. */
    *(uint32_t *)(out_entries + pos * ASNIX_ATTR_SIZE + ASNIX_ATTR_OFF_SYM) = sym;
    *(Value *)(out_entries + pos * ASNIX_ATTR_SIZE + ASNIX_ATTR_OFF_VALUE) = value;
    if (pos < acount) {
        memcpy(out_entries + (pos + 1) * ASNIX_ATTR_SIZE, aentries + pos * ASNIX_ATTR_SIZE,
               (size_t)(acount - pos) * ASNIX_ATTR_SIZE);
    }
    *(int64_t *)((char *)mem + ASNIX_ATTRS_OFF_COUNT) = acount + 1;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_ATTRS };
}
