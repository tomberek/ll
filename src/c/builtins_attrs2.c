/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
/*
 * filterAttrs, listToAttrs, attrByPath -- see builtins_attrs2.h. Verified
 * against real `nix-instantiate --eval` (2.35.2) before being trusted;
 * see each function's own comment for the specific expressions checked.
 */
#include "builtins_attrs2.h"

#include <stdint.h>
#include <stdlib.h>

#include "ll/art_sym.h"
#include "ll/heap.h"
#include "ll/ir.h"
#include "builtins_shared.h"
#include "symtab.h"
#include "value_util.h"

/* ---------------------------------------------------------------------
 * filterAttrs pred set -- verified: `builtins.filterAttrs (n: v: v > 1)
 * { a = 1; b = 2; }` == { b = 2; }. `pred` is called as `pred name value`
 * (two successive ll_apply calls, matching every other curried-call-a-
 * user-function builtin here) with the entry's *unforced* raw value --
 * same lazy-pass-through-as-a-call-argument idiom builtins_attrs.c's own
 * mapAttrs already relies on (the callee's own OP_VAR_REF forces it the
 * moment its body actually reads the parameter -- see M6's "forcing
 * happens inline in OP_VAR_REF/OP_SELECT" design, docs/architecture.md).
 * Kept entries retain `set`'s existing ascending-sym-id order (only
 * filtering, never reordering -- same invariant removeAttrs relies on).
 * --------------------------------------------------------------------- */
static Value filterAttrs2_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value pred = args[0], set = args[1];
    LL_REQUIRE_ATTRS(set, "filterAttrs");
    int64_t n = ll_bshared_attrs_count(set);
    uint32_t *syms = n > 0 ? malloc((size_t)n * sizeof(uint32_t)) : NULL;
    Value *vals = n > 0 ? malloc((size_t)n * sizeof(Value)) : NULL;
    int64_t out_n = 0;
    for (int64_t i = 0; i < n; i++) {
        uint32_t sym = ll_bshared_attrs_sym(set, i);
        size_t len;
        const char *name = ll_sym_name(sym, &len);
        Value name_val = ll_make_string_value(name, len);
        Value orig = ll_bshared_attrs_value(set, i);
        Value keep = ll_bshared_apply2(pred, name_val, orig);
        if (ll_tag_of(keep) != LL_TAG_IMM ||
            (ll_imm_subtag(keep) != LL_IMM_TRUE && ll_imm_subtag(keep) != LL_IMM_FALSE)) {
            ll_bshared_fatal("filterAttrs", "predicate did not return a boolean");
        }
        if (ll_imm_subtag(keep) == LL_IMM_TRUE) {
            syms[out_n] = sym;
            vals[out_n] = orig;
            out_n++;
        }
    }
    /* `syms`/`vals` already ascend (inherited from `set`'s own order, only
     * filtered in a single forward pass) -- ll_bshared_make_attrs's own
     * sort is a no-op here, same as removeAttrs's established argument. */
    Value result = ll_bshared_make_attrs(syms, vals, out_n);
    free(syms);
    free(vals);
    return result;
}
static Value build_filterAttrs(void) { return ll_bshared_make_curried_native((void *)filterAttrs2_impl, 2); }

/* M196 perf: listToAttrs's own dedupe used to be an unconditional O(n^2)
 * linear scan over every already-collected output entry for every list
 * element -- the same anti-pattern M89/M158 already fixed in make_attrs
 * and M195 fixed in zipAttrsWith, just missed here since it's a separate,
 * inline check in a different file, and (unlike those two) had no size
 * threshold at all.
 *
 * M198: noticed that listToAttrs's dedupe ("first occurrence of a
 * duplicate name wins") and zipAttrsWith's symbol collection are the
 * SAME problem: both need "insert a sym, and if it's already present,
 * keep whatever was there first". That is EXACTLY ll_art_sym_insert's
 * own semantics once it carries a Value payload (art_sym.h/.c) -- a
 * duplicate insert is a complete no-op, so the first-ever-inserted value
 * is always what survives. So for large lists (> the same 64-entry
 * threshold zipAttrsWith2_impl uses, same measured crossover -- see
 * art_sym.h's header comment for the numbers), skip the separate
 * collect+sort+dedupe passes entirely: insert each (sym, value) directly
 * into the ART during the single pass over list elements (every element
 * needs forcing + name/value lookup regardless), then one in-order
 * traversal yields the final sorted, deduped (sym, value) arrays
 * make_attrs needs, with no sort and no dedupe pass at all. Below the
 * threshold, keep the plain collect + insertion-sort(by sym, idx tie-
 * break for stability) + single dedupe pass from the original M196 fix
 * -- cheaper than ART's per-key node allocation/traversal overhead at
 * this size. */
#define LL_LIST_TO_ATTRS_ART_THRESHOLD 64

typedef struct { uint32_t sym; int64_t idx; Value val; } ListToAttrsEntry;

/* ---------------------------------------------------------------------
 * listToAttrs list -- verified:
 * `builtins.listToAttrs [{name="foo"; value=123;} {name="bar"; value=456;}
 * {name="bar"; value=420;}]` == { bar = 456; foo = 123; } -- first
 * occurrence of a duplicate name wins (confirmed directly: the result's
 * `bar` is 456, from the *first* `{name="bar";...}`, not 420 from the
 * second). Each list element must be an attrset with a string `name` and
 * a `value`; `value` is passed through unforced (same lazy pass-through
 * as catAttrs/getAttr above).
 * --------------------------------------------------------------------- */
static Value prim_listToAttrs(const Value *args, int64_t argc) {
    (void)argc;
    Value list = args[0];
    LL_REQUIRE_LIST(list, "listToAttrs");
    int64_t n = ll_bshared_list_count(list);
    const Value *elems = ll_bshared_list_elems(list);

    uint32_t sym_name = ll_intern("name", 4);
    uint32_t sym_value = ll_intern("value", 5);

    if (n > LL_LIST_TO_ATTRS_ART_THRESHOLD) {
        LlArtSymSet set;
        ll_art_sym_init(&set);
        for (int64_t i = 0; i < n; i++) {
            /* Lazy list elements (M23's `map`, etc.) -- force before
             * checking the tag, same class of fix M23's own concatLists
             * needed. */
            Value elem = ll_force_for_print(elems[i]);
            LL_REQUIRE_ATTRS(elem, "listToAttrs");
            int64_t name_idx = ll_bshared_attrs_find(elem, sym_name);
            if (name_idx < 0) ll_bshared_fatal("listToAttrs", "an element is missing the 'name' attribute");
            int64_t value_idx = ll_bshared_attrs_find(elem, sym_value);
            if (value_idx < 0) ll_bshared_fatal("listToAttrs", "an element is missing the 'value' attribute");

            Value name_v = ll_force_for_print(ll_bshared_attrs_value(elem, name_idx));
            LL_REQUIRE_STRING(name_v, "listToAttrs");
            int64_t nlen;
            const char *nbytes;
            ll_bshared_string_bytes(name_v, &nbytes, &nlen, "listToAttrs");
            uint32_t sym = ll_intern(nbytes, (size_t)nlen);

            ll_art_sym_insert(&set, sym, ll_bshared_attrs_value(elem, value_idx));
        }
        uint32_t *syms = malloc((size_t)n * sizeof(uint32_t));
        Value *vals = malloc((size_t)n * sizeof(Value));
        int64_t out_n = ll_art_sym_collect_sorted(&set, syms, vals);
        Value result = ll_bshared_make_attrs(syms, vals, out_n);
        free(syms);
        free(vals);
        return result;
    }

    ListToAttrsEntry *entries = n > 0 ? malloc((size_t)n * sizeof(ListToAttrsEntry)) : NULL;

    for (int64_t i = 0; i < n; i++) {
        /* Lazy list elements (M23's `map`, etc.) -- force before checking
         * the tag, same class of fix M23's own concatLists needed. */
        Value elem = ll_force_for_print(elems[i]);
        LL_REQUIRE_ATTRS(elem, "listToAttrs");
        int64_t name_idx = ll_bshared_attrs_find(elem, sym_name);
        if (name_idx < 0) ll_bshared_fatal("listToAttrs", "an element is missing the 'name' attribute");
        int64_t value_idx = ll_bshared_attrs_find(elem, sym_value);
        if (value_idx < 0) ll_bshared_fatal("listToAttrs", "an element is missing the 'value' attribute");

        Value name_v = ll_force_for_print(ll_bshared_attrs_value(elem, name_idx));
        LL_REQUIRE_STRING(name_v, "listToAttrs");
        int64_t nlen;
        const char *nbytes;
        ll_bshared_string_bytes(name_v, &nbytes, &nlen, "listToAttrs");
        uint32_t sym = ll_intern(nbytes, (size_t)nlen);

        entries[i].sym = sym;
        entries[i].idx = i;
        entries[i].val = ll_bshared_attrs_value(elem, value_idx);
    }

    /* n <= LL_LIST_TO_ATTRS_ART_THRESHOLD(64) here, so a plain
     * insertion sort is always cheap enough -- no qsort tier needed. */
    for (int64_t i = 1; i < n; i++) {
        int64_t j = i;
        ListToAttrsEntry e = entries[i];
        while (j > 0 && (entries[j - 1].sym > e.sym ||
                         (entries[j - 1].sym == e.sym && entries[j - 1].idx > e.idx))) {
            entries[j] = entries[j - 1];
            j--;
        }
        entries[j] = e;
    }

    uint32_t *syms = n > 0 ? malloc((size_t)n * sizeof(uint32_t)) : NULL;
    Value *vals = n > 0 ? malloc((size_t)n * sizeof(Value)) : NULL;
    int64_t out_n = 0;
    for (int64_t i = 0; i < n; i++) {
        if (out_n > 0 && syms[out_n - 1] == entries[i].sym) continue; /* later (by idx) duplicate: first occurrence already kept */
        syms[out_n] = entries[i].sym;
        vals[out_n] = entries[i].val;
        out_n++;
    }

    Value result = ll_bshared_make_attrs(syms, vals, out_n);
    free(entries);
    free(syms);
    free(vals);
    return result;
}
static Value build_listToAttrs(void) { return ll_bshared_make_primop((void *)prim_listToAttrs, 1); }

/* ---------------------------------------------------------------------
 * attrByPath path default set -- ported directly from real nixpkgs lib
 * (lib/attrsets.nix, confirmed via
 * `(builtins.unsafeGetAttrPos "attrByPath" lib).file`):
 *
 *   attrByPath = attrPath: default: set:
 *     let attrByPath' = n: s:
 *       if n == length attrPath then s
 *       else let attr = elemAt attrPath n;
 *            in if s ? ${attr} then attrByPath' (n+1) s.${attr} else default;
 *     in attrByPath' 0 set;
 *
 * i.e. walk the path one component at a time; `s ? attr` is `false` (not
 * an error) the moment `s` stops being an attrset, exactly like this
 * project's own `OP_HAS_ATTR` -- so a path that runs into a non-attrset
 * partway through just returns `default`, matching real Nix exactly
 * (verified: `lib.attrByPath ["a" "b"] 99 { a = {}; }` == 99;
 * `lib.attrByPath [] 99 { a = 1; }` == { a = 1; } -- the empty path
 * returns the whole set unchanged).
 * --------------------------------------------------------------------- */
static Value attrByPath3_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value path = args[0], deflt = args[1], set = args[2];
    LL_REQUIRE_LIST(path, "attrByPath");
    int64_t n = ll_bshared_list_count(path);
    const Value *comps = ll_bshared_list_elems(path);

    Value cur = set;
    for (int64_t i = 0; i < n; i++) {
        if (ll_tag_of(cur) != LL_TAG_ATTRS) return deflt;
        /* Lazy list elements (M23's `map`, etc.) -- force before checking
         * the tag, same class of fix M23's own concatLists needed. */
        Value comp = ll_force_for_print(comps[i]);
        LL_REQUIRE_STRING(comp, "attrByPath");
        int64_t clen;
        const char *cbytes;
        ll_bshared_string_bytes(comp, &cbytes, &clen, "attrByPath");
        uint32_t sym = ll_intern(cbytes, (size_t)clen);
        int64_t idx = ll_bshared_attrs_find(cur, sym);
        if (idx < 0) return deflt;
        cur = ll_force_for_print(ll_bshared_attrs_value(cur, idx));
    }
    return cur;
}
static Value build_attrByPath(void) { return ll_bshared_make_curried_native((void *)attrByPath3_impl, 3); }

const LlBuiltinMiscDef LL_ATTRS2_BUILTINS[] = {
    { "filterAttrs", build_filterAttrs },
    { "listToAttrs", build_listToAttrs },
    { "attrByPath", build_attrByPath },
};

const size_t LL_ATTRS2_BUILTINS_COUNT = sizeof(LL_ATTRS2_BUILTINS) / sizeof(LL_ATTRS2_BUILTINS[0]);
