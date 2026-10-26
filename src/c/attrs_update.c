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

/* `a // b`, matching iets's own `update_attrs`/`alloc_union` heuristic
 * (src/eval/mod.rs, src/runtime/value.rs) rather than asnix's original
 * M88 design -- three differences from a naive "just defer small RHS
 * onto big LHS" rule, all three needed together to pay off:
 *
 *   1. Empty-operand short circuit: `x // {}` and `{} // x` return the
 *      other side unchanged, with no allocation at all. Real nixpkgs/
 *      stdenv code does this constantly (optional overrides, `lib.
 *      optionalAttrs false {...}`, default-argument attrsets).
 *
 *   2. `rhs` is never eagerly resolved -- only flattened if deferring
 *      turns out not to fit the leaf budget below. A `//` whose RHS is
 *      itself a still-deferred union from an earlier expression (overlay
 *      composition) gets its own leaves folded in directly, unresolved.
 *
 *   3. On leaf-budget overflow, flatten ONLY `lhs` down to one leaf and
 *      retry as a fresh, short union, instead of resolving all of `lhs`
 *      and eager-merging both sides. This is the piece a naive port of
 *      iets's MAX_LEAVES=8 constant onto the OLD restart strategy didn't
 *      have -- measured clearly worse on its own (see git history), since
 *      it just paid the old expensive fallback 8x more often. With the
 *      cheap restart, the expensive two-sided eager merge below becomes
 *      rare: once every ~8 leaves lhs resolves itself to one leaf, not a
 *      full flatten-and-copy of both operands.
 *
 * Measured on the real ffmpeg-full.outPath benchmark against asnix's own
 * RHS_MAX=16/LHS_MIN=16/MAX_LEAVES=64 design: true attrset-byte total
 * 116.6MB -> 84.6MB, instructions +0.345%, peak RSS -6.3%, wall clock no
 * measured change. */
#define LL_UNION_A_SMALL_MAX 8
#define LL_UNION_MAX_LEAVES 8

/* Combines lhs's own leaves (if lhs is a union) or lhs itself (if flat)
 * with rhs's own leaves (if rhs is a union, unpacked the same way --
 * matching iets's `for_each_leaf`) or rhs itself (if flat), in priority
 * order (lhs's leaves first, rhs's leaves last -- rhs wins ties). */
static Value make_union_from_both(Value lhs, Value rhs) {
    const char *lbase = (const char *)(lhs.raw & ~0x7ULL);
    int64_t lraw = *(const int64_t *)(lbase + ASNIX_ATTRS_OFF_COUNT);
    int lhs_is_union = lraw < 0;
    int64_t n_lhs = lhs_is_union ? -lraw : 1;
    const Value *lhs_leaves = lhs_is_union ? (const Value *)(lbase + ASNIX_ATTRS_UNION_OFF_LEAVES) : NULL;

    const char *rbase = (const char *)(rhs.raw & ~0x7ULL);
    int64_t rraw = *(const int64_t *)(rbase + ASNIX_ATTRS_OFF_COUNT);
    int rhs_is_union = rraw < 0;
    int64_t n_rhs = rhs_is_union ? -rraw : 1;
    const Value *rhs_leaves = rhs_is_union ? (const Value *)(rbase + ASNIX_ATTRS_UNION_OFF_LEAVES) : NULL;

    int64_t nleaves = n_lhs + n_rhs;
    uint64_t sz = ASNIX_ATTRS_UNION_OFF_LEAVES + (uint64_t)nleaves * sizeof(Value);
    void *mem = asnix_arena_alloc(sz);
    *(int64_t *)((char *)mem + ASNIX_ATTRS_UNION_OFF_NLEAVES) = -nleaves;
    *(Value *)((char *)mem + ASNIX_ATTRS_UNION_OFF_FLATCACHE) = (Value){ .raw = 0 };
    Value *out = (Value *)((char *)mem + ASNIX_ATTRS_UNION_OFF_LEAVES);
    if (lhs_is_union) { memcpy(out, lhs_leaves, (size_t)n_lhs * sizeof(Value)); out += n_lhs; }
    else { *out++ = lhs; }
    if (rhs_is_union) { memcpy(out, rhs_leaves, (size_t)n_rhs * sizeof(Value)); out += n_rhs; }
    else { *out++ = rhs; }
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_ATTRS };
}

Value asnix_attrs_update(Value lhs, Value rhs) {
    const char *lhs_base = (const char *)(lhs.raw & ~0x7ULL);
    int64_t lhs_raw = *(const int64_t *)(lhs_base + ASNIX_ATTRS_OFF_COUNT);
    if (lhs_raw == 0) return rhs;

    const char *rhs_base = (const char *)(rhs.raw & ~0x7ULL);
    int64_t rhs_raw = *(const int64_t *)(rhs_base + ASNIX_ATTRS_OFF_COUNT);
    if (rhs_raw == 0) return lhs;

    int lhs_is_union = lhs_raw < 0;
    int64_t lhs_leaves = lhs_is_union ? -lhs_raw : 1;
    int a_small = !lhs_is_union && lhs_raw < LL_UNION_A_SMALL_MAX;

    if (!a_small) {
        int rhs_is_union = rhs_raw < 0;
        int64_t rhs_leaves = rhs_is_union ? -rhs_raw : 1;

        if (lhs_leaves + rhs_leaves > LL_UNION_MAX_LEAVES) {
            lhs = asnix_bshared_attrs_resolve(lhs);
            lhs_leaves = 1;
        }
        if (lhs_leaves + rhs_leaves <= LL_UNION_MAX_LEAVES) {
            return make_union_from_both(lhs, rhs);
        }
    }

    /* Fallback: both operands must be flat for the two-pointer merge. */
    Value lhs_flat = asnix_bshared_attrs_resolve(lhs);
    Value rhs_flat = asnix_bshared_attrs_resolve(rhs);
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
