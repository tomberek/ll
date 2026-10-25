/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
/*
 * elem, any, all, concatMap, sort, genList, partition, foldl, foldr,
 * catAttrs -- see builtins_list2.h. Every entry here is arity>=2 in real
 * Nix, so each is built via builtins_shared.h's make_curried_native (a
 * curried Closure chain, same mechanism builtins_misc.c's `substring`/
 * `concatStringsSep` already use), not a single multi-arg PrimOp
 * registration -- parser.c's parse_app always desugars `f a b` into
 * nested arity-1 App nodes, so OP_CALL only ever sees argc=1 calls for
 * ordinary Nix-level application (see docs/architecture.md's M8/M17
 * scope notes for the full rationale).
 *
 * Every builtin below was verified against real `nix-instantiate --eval`
 * (2.35.2) before being trusted -- see each function's own comment for the
 * specific expressions checked.
 */
#include "builtins_list2.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/heap.h"
#include "asnix/ir.h"
#include "builtins_shared.h"
#include "symtab.h"

static Value check_bool(Value v, const char *who) {
    if (asnix_tag_of(v) != ASNIX_TAG_IMM ||
        (asnix_imm_subtag(v) != ASNIX_IMM_TRUE && asnix_imm_subtag(v) != ASNIX_IMM_FALSE)) {
        asnix_bshared_fatal(who, "function did not return a boolean");
    }
    return v;
}

/* ---------------------------------------------------------------------
 * elem x list -- verified: `builtins.elem 2 [1 2 3]` == true,
 * `builtins.elem 4 [1 2 3]` == false. Equality is asnix_bshared_value_eq
 * (real content equality for int/string/bool/null, pointer identity for
 * aggregates -- see builtins_shared.h), not real Nix's deep structural
 * eqValues; a documented, narrow divergence, not an oversight.
 *
 * M92: `x` (args[0]) is deliberately left completely unforced -- built
 * via asnix_bshared_make_curried_native_noforce (NOT the ordinary
 * asnix_bshared_make_curried_native every other curried builtin here
 * uses), specifically so asnix_bshared_value_eq's own slot/Cell-identity
 * shortcut (see that function's M92 comment) still has something to
 * check: forcing `x` before this function ever sees it -- which is
 * exactly what the FORCE-mode curry builder's OP_VAR_REF reassembly
 * (plus op_call.S's own M25 post-dispatch force-loop) would otherwise
 * do, completely incidentally, to every curried argument -- throws away
 * the very slot-identity information `elem x [x]` depends on being
 * `true`. Matches real Nix's own `prim_elem` (primops.cc), which passes
 * `*args[0]` to `eqValues` with no forcing of its own either, for the
 * identical reason. `list` (args[1]) must still be forced explicitly
 * here, since NOFORCE means NEITHER argument arrives pre-forced. */
Value asnix_prim_elem2_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value x = args[0];
    Value list = asnix_force_for_print(args[1]);
    ASNIX_REQUIRE_LIST(list, "elem");
    int64_t n = asnix_bshared_list_count(list);
    const Value *elems = asnix_bshared_list_elems(list);
    for (int64_t i = 0; i < n; i++) {
        if (asnix_bshared_value_eq(x, elems[i])) return ASNIX_TRUE;
    }
    return ASNIX_FALSE;
}
static Value build_elem(void) { return asnix_bshared_make_curried_native_noforce((void *)asnix_prim_elem2_impl, 2); }

/* ---------------------------------------------------------------------
 * any pred list / all pred list -- verified:
 * `builtins.any (x: x > 2) [1 2 3]` == true;
 * `builtins.all (x: x > 0) [1 2 3]` == true. Both short-circuit, matching
 * real Nix exactly: `builtins.any (x: if x==2 then true else throw "x")
 * [2 1 3]` succeeds on real Nix (stops at the first `true`, never reaches
 * the throwing element) -- confirmed directly before relying on it.
 * --------------------------------------------------------------------- */
Value asnix_prim_any2_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value pred = args[0], list = args[1];
    ASNIX_REQUIRE_LIST(list, "any");
    int64_t n = asnix_bshared_list_count(list);
    const Value *elems = asnix_bshared_list_elems(list);
    for (int64_t i = 0; i < n; i++) {
        Value r = check_bool(asnix_apply(pred, &elems[i], 1), "any");
        if (asnix_imm_subtag(r) == ASNIX_IMM_TRUE) return ASNIX_TRUE;
    }
    return ASNIX_FALSE;
}
static Value build_any(void) { return asnix_bshared_make_curried_native((void *)asnix_prim_any2_impl, 2); }

Value asnix_prim_all2_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value pred = args[0], list = args[1];
    ASNIX_REQUIRE_LIST(list, "all");
    int64_t n = asnix_bshared_list_count(list);
    const Value *elems = asnix_bshared_list_elems(list);
    for (int64_t i = 0; i < n; i++) {
        Value r = check_bool(asnix_apply(pred, &elems[i], 1), "all");
        if (asnix_imm_subtag(r) == ASNIX_IMM_FALSE) return ASNIX_FALSE;
    }
    return ASNIX_TRUE;
}
static Value build_all(void) { return asnix_bshared_make_curried_native((void *)asnix_prim_all2_impl, 2); }

/* ---------------------------------------------------------------------
 * concatMap f list -- verified: `builtins.concatMap (x: [x x]) [1 2 3]`
 * == [1 1 2 2 3 3]. Equivalent to `concatLists (map f list)` but computed
 * directly (one pass calling f, one pass copying), same two-pass shape
 * builtins_list.c's own concatLists already uses.
 * --------------------------------------------------------------------- */
static Value concatMap2_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value f = args[0], list = args[1];
    ASNIX_REQUIRE_LIST(list, "concatMap");
    int64_t n = asnix_bshared_list_count(list);
    const Value *elems = asnix_bshared_list_elems(list);
    Value *subs = n > 0 ? malloc((size_t)n * sizeof(Value)) : NULL;
    int64_t total = 0;
    for (int64_t i = 0; i < n; i++) {
        subs[i] = asnix_apply(f, &elems[i], 1);
        ASNIX_REQUIRE_LIST(subs[i], "concatMap");
        total += asnix_bshared_list_count(subs[i]);
    }
    Value *out = total > 0 ? malloc((size_t)total * sizeof(Value)) : NULL;
    int64_t pos = 0;
    for (int64_t i = 0; i < n; i++) {
        int64_t c = asnix_bshared_list_count(subs[i]);
        if (c > 0) memcpy(out + pos, asnix_bshared_list_elems(subs[i]), (size_t)c * sizeof(Value));
        pos += c;
    }
    Value result = asnix_bshared_make_list(out, total);
    free(subs);
    free(out);
    return result;
}
static Value build_concatMap(void) { return asnix_bshared_make_curried_native((void *)concatMap2_impl, 2); }

/* ---------------------------------------------------------------------
 * genList f n -- verified: `builtins.genList (x: x*x) 5` == [0 1 4 9 16].
 * Computed eagerly (calls f immediately for every index) rather than real
 * Nix's lazy-per-element list (`<CODE>` when printed unforced there) --
 * consistent with this project's standing M6 scope decision that list
 * elements are not thunked (see docs/architecture.md), the same choice
 * `map`/`mapAttrs` already made for the identical reason.
 * --------------------------------------------------------------------- */
static Value genList2_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value f = args[0], n_v = args[1];
    ASNIX_REQUIRE_INT(n_v, "genList");
    int64_t n = asnix_int_value(n_v);
    if (n < 0) asnix_bshared_fatal("genList", "length must be non-negative");
    Value *out = n > 0 ? malloc((size_t)n * sizeof(Value)) : NULL;
    for (int64_t i = 0; i < n; i++) {
        Value iv = asnix_make_int(i);
        out[i] = asnix_apply(f, &iv, 1);
    }
    Value result = asnix_bshared_make_list(out, n);
    free(out);
    return result;
}
static Value build_genList(void) { return asnix_bshared_make_curried_native((void *)genList2_impl, 2); }

/* ---------------------------------------------------------------------
 * sort cmp list -- verified: `builtins.sort builtins.lessThan [3 1 2]`
 * == [1 2 3]. Stable insertion sort (preserves relative order of elements
 * the comparator treats as equivalent, matching real Nix's documented
 * stability guarantee) -- O(n^2), accepted the same way this project
 * already accepts it elsewhere for "not a hot path, typically small"
 * builtins (e.g. builtins_attrs.c's sorted_name_order); real Nix uses an
 * O(n log n) adaptive mergesort (peeksort) for the same job, which would
 * be a reasonable follow-up if a caller ever sorts something large enough
 * to care.
 * --------------------------------------------------------------------- */
static Value sort2_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value cmp = args[0], list = args[1];
    ASNIX_REQUIRE_LIST(list, "sort");
    int64_t n = asnix_bshared_list_count(list);
    if (n == 0) return list;
    Value *arr = malloc((size_t)n * sizeof(Value));
    memcpy(arr, asnix_bshared_list_elems(list), (size_t)n * sizeof(Value));
    for (int64_t i = 1; i < n; i++) {
        Value key = arr[i];
        int64_t j = i - 1;
        while (j >= 0) {
            Value lt = check_bool(asnix_bshared_apply2(cmp, key, arr[j]), "sort");
            if (asnix_imm_subtag(lt) != ASNIX_IMM_TRUE) break;
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
    Value result = asnix_bshared_make_list(arr, n);
    free(arr);
    return result;
}
static Value build_sort(void) { return asnix_bshared_make_curried_native((void *)sort2_impl, 2); }

/* ---------------------------------------------------------------------
 * partition pred list -- verified: `builtins.partition (x: x > 2)
 * [1 2 3 4]` == { right = [3 4]; wrong = [1 2]; }. The two freshly
 * interned symbols ("right"/"wrong") are sorted by sym id before building
 * the result Attrs (asnix_bshared_make_attrs) -- their relative sym-id
 * order depends on whatever else this process has interned already, not
 * source order, so it can't be assumed ascending the way removeAttrs/
 * mapAttrs can assume it from an already-sorted input Attrs.
 * --------------------------------------------------------------------- */
static Value partition2_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value pred = args[0], list = args[1];
    ASNIX_REQUIRE_LIST(list, "partition");
    int64_t n = asnix_bshared_list_count(list);
    const Value *elems = asnix_bshared_list_elems(list);
    Value *right = n > 0 ? malloc((size_t)n * sizeof(Value)) : NULL;
    Value *wrong = n > 0 ? malloc((size_t)n * sizeof(Value)) : NULL;
    int64_t nr = 0, nw = 0;
    for (int64_t i = 0; i < n; i++) {
        Value r = check_bool(asnix_apply(pred, &elems[i], 1), "partition");
        if (asnix_imm_subtag(r) == ASNIX_IMM_TRUE) right[nr++] = elems[i];
        else wrong[nw++] = elems[i];
    }
    uint32_t syms[2];
    Value vals[2];
    syms[0] = asnix_intern("right", 5);
    vals[0] = asnix_bshared_make_list(right, nr);
    syms[1] = asnix_intern("wrong", 5);
    vals[1] = asnix_bshared_make_list(wrong, nw);
    Value result = asnix_bshared_make_attrs(syms, vals, 2);
    free(right);
    free(wrong);
    return result;
}
static Value build_partition(void) { return asnix_bshared_make_curried_native((void *)partition2_impl, 2); }

/* ---------------------------------------------------------------------
 * foldl op nul list / foldr op nul list --
 *
 * Only `foldl'` (strict) is a core Nix builtin: confirmed directly,
 * `nix-instantiate --eval -E 'builtins ? foldl'` and `builtins ? foldr`
 * both report `false`; only `builtins ? "foldl'"` is `true`. Plain
 * `foldl`/`foldr` are nixpkgs *lib* functions (`lib.foldl`/`lib.foldr`),
 * not core builtins. Added here as `builtins.foldl`/`builtins.foldr`
 * anyway -- same choice this pass makes for `attrByPath`/
 * `stringToCharacters`/`filterAttrs` (also lib-only or lib-originated in
 * at least some Nix versions), since this project has no "with lib" root
 * scope the way real Nix programs reach `lib` at all; everything useful
 * has to live directly under `builtins` here or not be reachable.
 *
 * `foldl` is implemented identically to `foldl'`, verified byte-for-byte
 * against real `lib.foldl`: `lib.foldl (a: b: a - b) 0 [1 2 3]` == -6,
 * the same result `foldl'` gives for the same expression. This is not a
 * copy-paste oversight -- real Nix's `foldl`/`foldl'` differ *only* in
 * strictness (whether the accumulator's thunk chain is forced after each
 * step), and this project's M6 design already forces every thunk inline
 * on read (`OP_VAR_REF`/`OP_SELECT` -- see docs/architecture.md's M6
 * scope notes), so a Nix-level closure here never actually hands back an
 * *unforced* Thunk `Value` for `foldl` to defer in the first place --
 * there is no observable strict/lazy distinction left to implement.
 * Kept as its own function (not a literal alias for `foldl'`) purely so
 * `builtins.foldl` doesn't depend on builtins_list.c's internals, not
 * because the two bodies differ.
 * --------------------------------------------------------------------- */
static Value foldl3_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value op = args[0], acc = args[1], list = args[2];
    ASNIX_REQUIRE_LIST(list, "foldl");
    int64_t n = asnix_bshared_list_count(list);
    const Value *elems = asnix_bshared_list_elems(list);
    for (int64_t i = 0; i < n; i++) acc = asnix_bshared_apply2(op, acc, elems[i]);
    return acc;
}
static Value build_foldl(void) { return asnix_bshared_make_curried_native((void *)foldl3_impl, 3); }

/* foldr op nul [x1 x2 ... xn] == op x1 (op x2 (... (op xn nul))) --
 * verified: `lib.foldr (a: b: a - b) 0 [1 2 3]` == 2 (= 1 - (2 - (3 - 0))).
 * A plain right-to-left native loop, not real recursion -- no reason to
 * spend one native C stack frame per list element (see
 * docs/architecture.md's Known Bugs #7 for a *different*, already-known
 * native-recursion depth limit elsewhere in this codebase; no reason to
 * add a second one here when an iterative loop is just as simple and
 * gives the identical left-to-right-application result). */
static Value foldr3_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value op = args[0], acc = args[1], list = args[2];
    ASNIX_REQUIRE_LIST(list, "foldr");
    int64_t n = asnix_bshared_list_count(list);
    const Value *elems = asnix_bshared_list_elems(list);
    for (int64_t i = n - 1; i >= 0; i--) acc = asnix_bshared_apply2(op, elems[i], acc);
    return acc;
}
static Value build_foldr(void) { return asnix_bshared_make_curried_native((void *)foldr3_impl, 3); }

/* ---------------------------------------------------------------------
 * catAttrs attr list -- verified: `builtins.catAttrs "a"
 * [{a=1;} {b=0;} {a=2;}]` == [1 2]. Attrsets missing the named attribute
 * are skipped (not an error, matching real Nix exactly); each collected
 * value is passed through verbatim, unforced -- same lazy pass-through
 * convention builtins_attrs.c's getAttr/removeAttrs already use.
 * --------------------------------------------------------------------- */
static Value catAttrs2_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value name_v = args[0], list = args[1];
    ASNIX_REQUIRE_STRING(name_v, "catAttrs");
    ASNIX_REQUIRE_LIST(list, "catAttrs");
    int64_t nlen;
    const char *nbytes;
    asnix_bshared_string_bytes(name_v, &nbytes, &nlen, "catAttrs");
    uint32_t sym = asnix_intern(nbytes, (size_t)nlen);

    int64_t n = asnix_bshared_list_count(list);
    const Value *elems = asnix_bshared_list_elems(list);
    Value *out = n > 0 ? malloc((size_t)n * sizeof(Value)) : NULL;
    int64_t nout = 0;
    for (int64_t i = 0; i < n; i++) {
        /* List elements are lazy (a `map`-produced list's elements are
         * deliberately unforced Thunks, M23) -- force before checking the
         * tag, same fix M23's own concatLists needed (see its comment,
         * builtins_list.c). */
        Value elem = asnix_force_for_print(elems[i]);
        ASNIX_REQUIRE_ATTRS(elem, "catAttrs");
        int64_t idx = asnix_bshared_attrs_find(elem, sym);
        if (idx >= 0) out[nout++] = asnix_bshared_attrs_value(elem, idx);
    }
    Value result = asnix_bshared_make_list(out, nout);
    free(out);
    return result;
}
static Value build_catAttrs(void) { return asnix_bshared_make_curried_native((void *)catAttrs2_impl, 2); }

const AsnixBuiltinMiscDef ASNIX_LIST2_BUILTINS[] = {
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

const size_t ASNIX_LIST2_BUILTINS_COUNT = sizeof(ASNIX_LIST2_BUILTINS) / sizeof(ASNIX_LIST2_BUILTINS[0]);
