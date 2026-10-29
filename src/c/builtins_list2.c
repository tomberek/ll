/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "builtins_list2.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ll/heap.h"
#include "ll/ir.h"
#include "builtins_shared.h"
#include "symtab.h"

static Value check_bool(Value v, const char *who) {
    if (ll_tag_of(v) != LL_TAG_IMM ||
        (ll_imm_subtag(v) != LL_IMM_TRUE && ll_imm_subtag(v) != LL_IMM_FALSE)) {
        ll_bshared_fatal(who, "function did not return a boolean");
    }
    return v;
}

/* ---------------------------------------------------------------------
 * elem x list
 *
 * `x` (args[0]) is deliberately left completely unforced -- built via
 * ll_bshared_make_curried_native_noforce, NOT the ordinary
 * ll_bshared_make_curried_native every other curried builtin here uses.
 * Do not switch this to the ordinary force-mode curry builder: forcing
 * `x` before this function sees it throws away the slot-identity
 * information `elem x [x]` depends on being `true` (ll_bshared_value_eq
 * has its own slot/Cell-identity shortcut that needs `x` to still be
 * the original, unforced reference). `list` (args[1]) must still be
 * forced explicitly here, since NOFORCE means NEITHER argument arrives
 * pre-forced. */
Value ll_prim_elem2_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value x = args[0];
    Value list = ll_force_for_print(args[1]);
    LL_REQUIRE_LIST(list, "elem");
    int64_t n = ll_bshared_list_count(list);
    const Value *elems = ll_bshared_list_elems(list);
    for (int64_t i = 0; i < n; i++) {
        if (ll_bshared_value_eq(x, elems[i])) return LL_TRUE;
    }
    return LL_FALSE;
}
static Value build_elem(void) { return ll_bshared_make_curried_native_noforce((void *)ll_prim_elem2_impl, 2); }

Value ll_prim_any2_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value pred = args[0], list = args[1];
    LL_REQUIRE_LIST(list, "any");
    int64_t n = ll_bshared_list_count(list);
    const Value *elems = ll_bshared_list_elems(list);
    for (int64_t i = 0; i < n; i++) {
        Value r = check_bool(ll_apply(pred, &elems[i], 1), "any");
        if (ll_imm_subtag(r) == LL_IMM_TRUE) return LL_TRUE;
    }
    return LL_FALSE;
}
static Value build_any(void) { return ll_bshared_make_curried_native((void *)ll_prim_any2_impl, 2); }

Value ll_prim_all2_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value pred = args[0], list = args[1];
    LL_REQUIRE_LIST(list, "all");
    int64_t n = ll_bshared_list_count(list);
    const Value *elems = ll_bshared_list_elems(list);
    for (int64_t i = 0; i < n; i++) {
        Value r = check_bool(ll_apply(pred, &elems[i], 1), "all");
        if (ll_imm_subtag(r) == LL_IMM_FALSE) return LL_FALSE;
    }
    return LL_TRUE;
}
static Value build_all(void) { return ll_bshared_make_curried_native((void *)ll_prim_all2_impl, 2); }

static Value concatMap2_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value f = args[0], list = args[1];
    LL_REQUIRE_LIST(list, "concatMap");
    int64_t n = ll_bshared_list_count(list);
    const Value *elems = ll_bshared_list_elems(list);
    Value *subs = n > 0 ? malloc((size_t)n * sizeof(Value)) : NULL;
    int64_t total = 0;
    for (int64_t i = 0; i < n; i++) {
        subs[i] = ll_apply(f, &elems[i], 1);
        LL_REQUIRE_LIST(subs[i], "concatMap");
        total += ll_bshared_list_count(subs[i]);
    }
    Value *out = total > 0 ? malloc((size_t)total * sizeof(Value)) : NULL;
    int64_t pos = 0;
    for (int64_t i = 0; i < n; i++) {
        int64_t c = ll_bshared_list_count(subs[i]);
        if (c > 0) memcpy(out + pos, ll_bshared_list_elems(subs[i]), (size_t)c * sizeof(Value));
        pos += c;
    }
    Value result = ll_bshared_make_list(out, total);
    free(subs);
    free(out);
    return result;
}
static Value build_concatMap(void) { return ll_bshared_make_curried_native((void *)concatMap2_impl, 2); }

static Value genList2_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value f = args[0], n_v = args[1];
    LL_REQUIRE_INT(n_v, "genList");
    int64_t n = ll_int_value(n_v);
    if (n < 0) ll_bshared_fatal("genList", "length must be non-negative");
    Value *out = n > 0 ? malloc((size_t)n * sizeof(Value)) : NULL;
    for (int64_t i = 0; i < n; i++) {
        Value iv = ll_make_int(i);
        out[i] = ll_apply(f, &iv, 1);
    }
    Value result = ll_bshared_make_list(out, n);
    free(out);
    return result;
}
static Value build_genList(void) { return ll_bshared_make_curried_native((void *)genList2_impl, 2); }

/* Stable insertion sort -- preserves relative order of elements the
 * comparator treats as equivalent, matching real Nix's documented
 * stability guarantee. */
static Value sort2_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value cmp = args[0], list = args[1];
    LL_REQUIRE_LIST(list, "sort");
    int64_t n = ll_bshared_list_count(list);
    if (n == 0) return list;
    Value *arr = malloc((size_t)n * sizeof(Value));
    memcpy(arr, ll_bshared_list_elems(list), (size_t)n * sizeof(Value));
    for (int64_t i = 1; i < n; i++) {
        Value key = arr[i];
        int64_t j = i - 1;
        while (j >= 0) {
            Value lt = check_bool(ll_bshared_apply2(cmp, key, arr[j]), "sort");
            if (ll_imm_subtag(lt) != LL_IMM_TRUE) break;
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
    Value result = ll_bshared_make_list(arr, n);
    free(arr);
    return result;
}
static Value build_sort(void) { return ll_bshared_make_curried_native((void *)sort2_impl, 2); }

static Value partition2_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value pred = args[0], list = args[1];
    LL_REQUIRE_LIST(list, "partition");
    int64_t n = ll_bshared_list_count(list);
    const Value *elems = ll_bshared_list_elems(list);
    Value *right = n > 0 ? malloc((size_t)n * sizeof(Value)) : NULL;
    Value *wrong = n > 0 ? malloc((size_t)n * sizeof(Value)) : NULL;
    int64_t nr = 0, nw = 0;
    for (int64_t i = 0; i < n; i++) {
        Value r = check_bool(ll_apply(pred, &elems[i], 1), "partition");
        if (ll_imm_subtag(r) == LL_IMM_TRUE) right[nr++] = elems[i];
        else wrong[nw++] = elems[i];
    }
    uint32_t syms[2];
    Value vals[2];
    syms[0] = ll_intern("right", 5);
    vals[0] = ll_bshared_make_list(right, nr);
    syms[1] = ll_intern("wrong", 5);
    vals[1] = ll_bshared_make_list(wrong, nw);
    Value result = ll_bshared_make_attrs(syms, vals, 2);
    free(right);
    free(wrong);
    return result;
}
static Value build_partition(void) { return ll_bshared_make_curried_native((void *)partition2_impl, 2); }

/* `foldl` is implemented identically to `foldl'` -- not a copy-paste
 * bug: real Nix's `foldl`/`foldl'` differ only in strictness, and this
 * project forces every thunk inline on read, so there's no observable
 * strict/lazy distinction left to implement. */
static Value foldl3_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value op = args[0], acc = args[1], list = args[2];
    LL_REQUIRE_LIST(list, "foldl");
    int64_t n = ll_bshared_list_count(list);
    const Value *elems = ll_bshared_list_elems(list);
    for (int64_t i = 0; i < n; i++) acc = ll_bshared_apply2(op, acc, elems[i]);
    return acc;
}
static Value build_foldl(void) { return ll_bshared_make_curried_native((void *)foldl3_impl, 3); }

/* foldr op nul [x1 x2 ... xn] == op x1 (op x2 (... (op xn nul))) --
 * implemented as a plain right-to-left native loop, not recursion. */
static Value foldr3_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value op = args[0], acc = args[1], list = args[2];
    LL_REQUIRE_LIST(list, "foldr");
    int64_t n = ll_bshared_list_count(list);
    const Value *elems = ll_bshared_list_elems(list);
    for (int64_t i = n - 1; i >= 0; i--) acc = ll_bshared_apply2(op, elems[i], acc);
    return acc;
}
static Value build_foldr(void) { return ll_bshared_make_curried_native((void *)foldr3_impl, 3); }

static Value catAttrs2_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value name_v = args[0], list = args[1];
    LL_REQUIRE_STRING(name_v, "catAttrs");
    LL_REQUIRE_LIST(list, "catAttrs");
    int64_t nlen;
    const char *nbytes;
    ll_bshared_string_bytes(name_v, &nbytes, &nlen, "catAttrs");
    uint32_t sym = ll_intern(nbytes, (size_t)nlen);

    int64_t n = ll_bshared_list_count(list);
    const Value *elems = ll_bshared_list_elems(list);
    Value *out = n > 0 ? malloc((size_t)n * sizeof(Value)) : NULL;
    int64_t nout = 0;
    for (int64_t i = 0; i < n; i++) {
        /* List elements are lazy -- force before checking the tag
         * (same reason as concatLists, builtins_list.c). */
        Value elem = ll_force_for_print(elems[i]);
        LL_REQUIRE_ATTRS(elem, "catAttrs");
        int64_t idx = ll_bshared_attrs_find(elem, sym);
        if (idx >= 0) out[nout++] = ll_bshared_attrs_value(elem, idx);
    }
    Value result = ll_bshared_make_list(out, nout);
    free(out);
    return result;
}
static Value build_catAttrs(void) { return ll_bshared_make_curried_native((void *)catAttrs2_impl, 2); }

const LlBuiltinMiscDef LL_LIST2_BUILTINS[] = {
    { "elem", build_elem },
    { "any", build_any },
    { "all", build_all },
    { "concatMap", build_concatMap },
    { "genList", build_genList },
    { "sort", build_sort },
    { "partition", build_partition },
    { "foldl", build_foldl },
    { "foldr", build_foldr },
    { "catAttrs", build_catAttrs },
};

const size_t LL_LIST2_BUILTINS_COUNT = sizeof(LL_LIST2_BUILTINS) / sizeof(LL_LIST2_BUILTINS[0]);
