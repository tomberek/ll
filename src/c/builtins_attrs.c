/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "builtins_attrs.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/env.h"
#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/irbuild.h"
#include "builtins_shared.h"
#include "symtab.h"
#include "value_util.h"

/* ---- small shared helpers (mirrors builtins.c's local conventions) ---- */

static Value make_primop_value(void *fn, int64_t arity) {
    void *mem = asnix_arena_alloc(ASNIX_PRIMOP_SIZE);
    *(void **)((char *)mem + ASNIX_PRIMOP_OFF_FN) = fn;
    *(int64_t *)((char *)mem + ASNIX_PRIMOP_OFF_ARITY) = arity;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_PRIMOP };
}

static void require_tag(Value v, int tag, const char *who, const char *what) {
    if (asnix_tag_of(v) != tag) {
        fprintf(stderr, "asnix: %s: argument is not %s\n", who, what);
        exit(1);
    }
}
#define require_attrs(v, who)  require_tag((v), ASNIX_TAG_ATTRS, (who), "an attrset")
#define require_string(v, who) require_tag((v), ASNIX_TAG_PTR, (who), "a string")
#define require_list(v, who)   require_tag((v), ASNIX_TAG_LIST, (who), "a list")

/* Raw heap-struct accessors matching include/asnix/heap.h's documented
 * layout -- same inline-pointer-arithmetic style builtins.c already uses
 * for String/List; factored into named helpers here since this file reads
 * Attrs/List/String structure in many more places. */
/* M88: `v` may be a lazy union header (heap.h) rather than a flat Attrs --
 * resolve to the flat form first (a single sign-bit check when it's
 * already flat, the common case) before poking raw layout. Every one of
 * this file's six builtins (attrNames/attrValues/hasAttr/getAttr/
 * removeAttrs/mapAttrs) goes through these three helpers, so fixing them
 * here covers the whole file. */
static int64_t attrs_count(Value v) {
    v = asnix_bshared_attrs_resolve(v);
    return *(const int64_t *)((const char *)(v.raw & ~0x7ULL) + ASNIX_ATTRS_OFF_COUNT);
}
static uint32_t attrs_entry_sym(Value v, int64_t i) {
    v = asnix_bshared_attrs_resolve(v);
    const char *entry = (const char *)(v.raw & ~0x7ULL) + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
    return *(const uint32_t *)(entry + ASNIX_ATTR_OFF_SYM);
}
static Value attrs_entry_value(Value v, int64_t i) {
    v = asnix_bshared_attrs_resolve(v);
    const char *entry = (const char *)(v.raw & ~0x7ULL) + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
    return *(const Value *)(entry + ASNIX_ATTR_OFF_VALUE);
}
static int64_t list_count(Value v) {
    return *(const int64_t *)((const char *)(v.raw & ~0x7ULL) + ASNIX_LIST_OFF_COUNT);
}
static Value list_elem(Value v, int64_t i) {
    return *(const Value *)((const char *)(v.raw & ~0x7ULL) + ASNIX_LIST_OFF_ELEMS + i * 8);
}
static Value make_list(const Value *elems, int64_t count) {
    void *mem = asnix_arena_alloc(8 + (uint64_t)count * 8);
    *(int64_t *)((char *)mem + ASNIX_LIST_OFF_COUNT) = count;
    if (count > 0) memcpy((char *)mem + ASNIX_LIST_OFF_ELEMS, elems, (size_t)count * 8);
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_LIST };
}
/* `syms`/`vals` must already be in ascending-sym order (same invariant
 * OP_SELECT's binary search and heap.h's "Attrs: ... sorted by sym" note
 * require) -- every caller below either inherits that order from an
 * existing Attrs object (removeAttrs, mapAttrs, both of which only filter/
 * replace-in-place without reordering) or sorts explicitly first. */
static Value make_attrs_from_sorted(const uint32_t *syms, const Value *vals, int64_t count) {
    void *mem = asnix_arena_alloc(8 + (uint64_t)count * ASNIX_ATTR_SIZE);
    *(int64_t *)((char *)mem + ASNIX_ATTRS_OFF_COUNT) = count;
    for (int64_t i = 0; i < count; i++) {
        char *entry = (char *)mem + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
        /* M103: must be a 4-byte write -- entry's value bytes now start right after sym (no pad),
         * so an 8-byte write here would clobber them. */
        *(uint32_t *)(entry + ASNIX_ATTR_OFF_SYM) = syms[i];
        *(Value *)(entry + ASNIX_ATTR_OFF_VALUE) = vals[i];
    }
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_ATTRS };
}

static int64_t string_len(Value v) {
    return *(const int64_t *)((const char *)(v.raw & ~0x7ULL) + ASNIX_STRING_OFF_LEN);
}
static const char *string_bytes(Value v) {
    return (const char *)(v.raw & ~0x7ULL) + ASNIX_STRING_OFF_BYTES;
}

static int name_cmp(const char *an, size_t al, const char *bn, size_t bl) {
    size_t n = al < bl ? al : bl;
    int c = memcmp(an, bn, n);
    if (c != 0) return c;
    return (al > bl) - (al < bl);
}

/* Perf note (post-M86 CPU profiling pass): this used to be a plain
 * insertion sort, on the theory ("attrsets are typically small and this
 * isn't a hot path") that's true for a hand-written literal but false
 * for the real thing `attrNames`/`attrValues` actually gets called on --
 * real nixpkgs's own top-level `pkgs` attrset has over 28,000 entries
 * (the same set `builtins_m23.c`'s `intersectAttrs` fix this session
 * measured directly), and `perf record -g` on `./asnix d.nix` (ffmpeg's
 * closure) showed `prim_attrNames`/`sorted_name_order`/`__memcmp_avx2_
 * movbe`/`asnix_sym_name` together consuming roughly a third of all
 * remaining cycles after this session's other two fixes (derivation.c's
 * closure cache, parser.c's btvec_find) -- an O(n^2) insertion sort over
 * tens of thousands of entries, each comparison re-resolving both sides'
 * symbol name from scratch via `asnix_sym_name`. Fixed two ways at once:
 * `qsort` (O(n log n) comparisons instead of O(n^2)), and resolving each
 * entry's name exactly ONCE up front into a flat array `qsort`'s
 * comparator reads directly, instead of once per comparison. */
typedef struct {
    int64_t idx;
    const char *name;
    size_t len;
} NameOrderEntry;

static int name_order_entry_cmp(const void *ap, const void *bp) {
    const NameOrderEntry *a = (const NameOrderEntry *)ap;
    const NameOrderEntry *b = (const NameOrderEntry *)bp;
    return name_cmp(a->name, a->len, b->name, b->len);
}

/* Builds `order[0..n-1]`, a permutation of attrs's entry indices sorted
 * alphabetically by attribute name -- real Nix's attrNames/attrValues
 * report attributes in alphabetical order, NOT this project's internal
 * Attr-array order (sorted by symbol id, which is assignment order, not
 * alphabetical -- see heap.h's note and docs/architecture.md's "Known
 * bugs" #4 about a real bug once caused by assuming otherwise). */
static void sorted_name_order(Value attrs, int64_t n, int64_t *order) {
    NameOrderEntry *entries = malloc((size_t)n * sizeof(NameOrderEntry));
    for (int64_t i = 0; i < n; i++) {
        size_t len;
        const char *name = asnix_sym_name(attrs_entry_sym(attrs, i), &len);
        entries[i] = (NameOrderEntry){ .idx = i, .name = name, .len = len };
    }
    qsort(entries, (size_t)n, sizeof(NameOrderEntry), name_order_entry_cmp);
    for (int64_t i = 0; i < n; i++) order[i] = entries[i].idx;
    free(entries);
}

static int64_t attrs_find(Value attrs, uint32_t sym) {
    int64_t lo = 0, hi = attrs_count(attrs);
    while (lo < hi) {
        int64_t mid = lo + (hi - lo) / 2;
        uint32_t s = attrs_entry_sym(attrs, mid);
        if (s == sym) return mid;
        if (s < sym) lo = mid + 1; else hi = mid;
    }
    return -1;
}

/* ---- curried-arity-2 plumbing ----
 *
 * hashString (builtins.c) curries its true-arity-2 signature by having
 * the first native stage pick one of a small STATIC set of native
 * functions (one of 4 known hash algorithms) and return that as the
 * second-stage PrimOp -- no captured runtime data needed, since "which
 * algorithm" is baked into which function pointer gets returned.
 *
 * hasAttr/getAttr/removeAttrs/mapAttrs can't use that trick: their first
 * argument is an arbitrary *runtime* value (a dynamic string, attrset, or
 * function), not one of a few statically-known options, and a native
 * PrimOp's fn gets no self/closure pointer to stash it in (see
 * docs/abi.md's "Primop calls" section, quoted in builtins.c). Instead,
 * this reuses the exact machinery a real two-argument user lambda
 * (`a: b: body`) already relies on: build a one-slot Env capturing the
 * first argument, and a Closure whose body is a 4-instruction IR snippet
 * (OP_PUSH_CONST the pre-built arity-2 PrimOp, OP_VAR_REF the captured
 * arg, OP_VAR_REF this closure's own arg, OP_CALL argc=2, OP_RET) --
 * built once with irbuild.c's existing IrBuilder API, not hand-assembled.
 * No new asm, no mutable global, no extra-data-after-the-struct trick.
 */
static Value make_curried2(Value captured, IrProgram *inner_prog) {
    Env *env = asnix_env_alloc(NULL, 1);
    env->values[0] = captured;
    void *mem = asnix_arena_alloc(ASNIX_CLOSURE_SIZE);
    *(void **)((char *)mem + ASNIX_CLOSURE_OFF_UP) = env;
    *(IrProgram **)((char *)mem + ASNIX_CLOSURE_OFF_PROG) = inner_prog; /* M100: one pointer, not {ops,pool} */
    *(int64_t *)((char *)mem + ASNIX_CLOSURE_OFF_ARITY) = 1;
    /* Never a lambda-pattern -- `builtins.functionArgs` on any of these
     * closures is `{}`, same as real Nix (M37). M80: the shared canonical
     * marker, not a fresh allocation -- see builtins_shared.h's
     * asnix_bshared_native_formals_marker. */
    *(uint64_t *)((char *)mem + ASNIX_CLOSURE_OFF_FORMALS) = asnix_bshared_native_formals_marker().raw;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_CLOSURE };
}

static IrProgram *build_curry2_body(Value primop2) {
    IrBuilder *b = ib_new();
    ib_push_const(b, (int64_t)primop2.raw);
    ib_var_ref(b, 1, 0); /* depth=1: the outer stage's captured first arg */
    ib_var_ref(b, 0, 0); /* depth=0: this (inner) closure's own arg */
    ib_call(b, 2);
    ib_ret(b);
    return ib_finish(b);
}

/* ---- attrNames / attrValues ---- */

static Value prim_attrNames(const Value *args, int64_t argc) {
    (void)argc;
    require_attrs(args[0], "attrNames");
    Value attrs = args[0];
    int64_t n = attrs_count(attrs);
    int64_t *order = asnix_arena_alloc((uint64_t)n * sizeof(int64_t));
    sorted_name_order(attrs, n, order);
    Value *out = asnix_arena_alloc((uint64_t)n * sizeof(Value));
    for (int64_t i = 0; i < n; i++) {
        size_t len;
        const char *name = asnix_sym_name(attrs_entry_sym(attrs, order[i]), &len);
        out[i] = asnix_make_string_value(name, len);
    }
    return make_list(out, n);
}

static Value prim_attrValues(const Value *args, int64_t argc) {
    (void)argc;
    require_attrs(args[0], "attrValues");
    Value attrs = args[0];
    int64_t n = attrs_count(attrs);
    int64_t *order = asnix_arena_alloc((uint64_t)n * sizeof(int64_t));
    sorted_name_order(attrs, n, order);
    Value *out = asnix_arena_alloc((uint64_t)n * sizeof(Value));
    for (int64_t i = 0; i < n; i++) out[i] = attrs_entry_value(attrs, order[i]);
    return make_list(out, n);
}

/* ---- hasAttr ---- */

static Value prim_hasAttr_apply2(const Value *args, int64_t argc) {
    (void)argc;
    require_string(args[0], "hasAttr");
    require_attrs(args[1], "hasAttr");
    uint32_t sym = asnix_intern(string_bytes(args[0]), (size_t)string_len(args[0]));
    return attrs_find(args[1], sym) >= 0 ? ASNIX_TRUE : ASNIX_FALSE;
}

static Value prim_hasAttr(const Value *args, int64_t argc) {
    (void)argc;
    static IrProgram *inner = NULL;
    if (!inner) inner = build_curry2_body(make_primop_value((void *)prim_hasAttr_apply2, 2));
    return make_curried2(args[0], inner);
}

/* ---- getAttr ---- */

static Value prim_getAttr_apply2(const Value *args, int64_t argc) {
    (void)argc;
    require_string(args[0], "getAttr");
    require_attrs(args[1], "getAttr");
    uint32_t sym = asnix_intern(string_bytes(args[0]), (size_t)string_len(args[0]));
    int64_t idx = attrs_find(args[1], sym);
    if (idx < 0) {
        fprintf(stderr, "asnix: getAttr: attribute '%.*s' missing\n",
                (int)string_len(args[0]), string_bytes(args[0]));
        exit(1);
    }
    return attrs_entry_value(args[1], idx);
}

static Value prim_getAttr(const Value *args, int64_t argc) {
    (void)argc;
    static IrProgram *inner = NULL;
    if (!inner) inner = build_curry2_body(make_primop_value((void *)prim_getAttr_apply2, 2));
    return make_curried2(args[0], inner);
}

/* ---- removeAttrs ---- */

static Value prim_removeAttrs_apply2(const Value *args, int64_t argc) {
    (void)argc;
    require_attrs(args[0], "removeAttrs");
    require_list(args[1], "removeAttrs");
    Value attrs = args[0];
    Value namelist = args[1];
    int64_t nnames = list_count(namelist);
    uint32_t *remove_syms = asnix_arena_alloc((uint64_t)nnames * sizeof(uint32_t));
    for (int64_t i = 0; i < nnames; i++) {
        /* M53: `namelist`'s elements can now be unforced Thunks -- list
         * literals (`removeAttrs set [ "b" ]`'s common shape) wrap each
         * element in its own thunk as of this milestone (compiler.c's
         * AST_LIST), the same way map/mapAttrs already did (M23). Force
         * before reading the raw tag/bytes, same fix M34 already applied
         * to the other direct list-element-reading builtins. */
        Value nv = asnix_force_for_print(list_elem(namelist, i));
        require_string(nv, "removeAttrs");
        remove_syms[i] = asnix_intern(string_bytes(nv), (size_t)string_len(nv));
    }
    int64_t n = attrs_count(attrs);
    uint32_t *out_syms = asnix_arena_alloc((uint64_t)n * sizeof(uint32_t));
    Value *out_vals = asnix_arena_alloc((uint64_t)n * sizeof(Value));
    int64_t out_n = 0;
    for (int64_t i = 0; i < n; i++) {
        uint32_t sym = attrs_entry_sym(attrs, i);
        int removed = 0;
        for (int64_t j = 0; j < nnames; j++) {
            if (remove_syms[j] == sym) { removed = 1; break; }
        }
        if (!removed) {
            out_syms[out_n] = sym;
            out_vals[out_n] = attrs_entry_value(attrs, i);
            out_n++;
        }
    }
    /* out_syms/out_vals inherit `attrs`'s own ascending-sym order since we
       only filter in a single forward pass, never reorder. */
    return make_attrs_from_sorted(out_syms, out_vals, out_n);
}

static Value prim_removeAttrs(const Value *args, int64_t argc) {
    (void)argc;
    static IrProgram *inner = NULL;
    if (!inner) inner = build_curry2_body(make_primop_value((void *)prim_removeAttrs_apply2, 2));
    return make_curried2(args[0], inner);
}

/* `removeAttrs` is one of real Nix's bare root-scope globals (verified
 * directly against `nix-instantiate --eval`, same research as M18's
 * import/map/abort/toString -- see docs/architecture.md) -- found
 * actually needed evaluating real nixpkgs (`pkgs/top-level/impure.nix`
 * calls bare `removeAttrs`, not `builtins.removeAttrs`). Wraps the exact
 * same `prim_removeAttrs` `builtins.removeAttrs` itself uses. */
Value asnix_make_removeAttrs_primop(void) {
    return make_primop_value((void *)prim_removeAttrs, 1);
}

/* ---- mapAttrs ----
 *
 * Real Nix's `builtins.mapAttrs f set` is lazy (each output value is a
 * fresh thunk wrapping `f name set.${name}`, only forced on demand).
 * M18-era version of this comment judged eager-vs-lazy "unobservable"
 * here since the printer forces everything anyway -- that reasoning was
 * wrong: numtide/nix-conformance's eval-okay-intersectAttrs.nix builds
 * `alphabetFail = builtins.mapAttrs throw alphabet` specifically as a
 * "canary" (every value throws when forced) and then only ever touches
 * its attribute *names*, never its values (via `intersectAttrs`) -- with
 * the old eager version, just *constructing* alphabetFail aborted
 * immediately, before intersectAttrs or the printer ever got a chance to
 * touch anything (confirmed directly: `builtins.attrNames
 * (builtins.mapAttrs throw { a = 1; })` aborted here but => `[ "a" ]` on
 * real Nix). Fixed the same way M23 also fixed plain `map`'s identical
 * bug (builtins_list.c's prim_map_stage2): each value is now a genuine
 * lazy value wrapping the curried `(f name) origvalue` call -- M118: a
 * ThunkMap (asnix_thunkmap_make_desc/_pack, value_util.c), one shared
 * descriptor for the whole call rather than M117's LazyApp (one 32-byte
 * heap object PER ELEMENT) -- see heap.h's ASNIX_THUNKMAP_MARKER comment
 * for the full rationale. `flat`'s own entries array is reused directly
 * as the descriptor's `src_entries` (no copy): `out`'s syms are copied
 * straight across from `flat`'s in the same order below, so `out`'s index
 * `i` and `flat`'s index `i` always name the same key, and the name
 * string itself is no longer built eagerly for every element up front --
 * only once a given element is actually forced (asnix_resolve_thunkmap). */
static Value prim_mapAttrs_apply2(const Value *args, int64_t argc) {
    (void)argc;
    Value f = args[0];
    Value attrs = args[1];
    require_attrs(attrs, "mapAttrs");
    Value flat = asnix_bshared_attrs_resolve(attrs); /* M88: resolve a lazy union ONCE here,
        rather than per-entry the way attrs_count/attrs_entry_sym/_value (used by this file's
        other builtins) each do independently -- src_entries below is only valid once flat. */
    const char *flat_base = (const char *)(flat.raw & ~0x7ULL);
    int64_t n = *(const int64_t *)(flat_base + ASNIX_ATTRS_OFF_COUNT);
    const char *src_entries = flat_base + ASNIX_ATTRS_OFF_ENTRIES;
    if (n == 0) return make_attrs_from_sorted(NULL, NULL, 0);

    uint32_t *syms = asnix_arena_alloc((uint64_t)n * sizeof(uint32_t));
    Value *vals = asnix_arena_alloc((uint64_t)n * sizeof(Value));

    if (n > ASNIX_THUNKMAP_MAX_IDX + 1) {
        /* Pathological: more entries than a packed ThunkMap index (16
         * bits) can address -- fall back to the old per-element LazyApp
         * path rather than silently truncating an index (see heap.h's
         * ASNIX_THUNKMAP_MARKER comment). */
        for (int64_t i = 0; i < n; i++) {
            const char *entry = src_entries + i * ASNIX_ATTR_SIZE;
            uint32_t sym = *(const uint32_t *)(entry + ASNIX_ATTR_OFF_SYM);
            Value orig = *(const Value *)(entry + ASNIX_ATTR_OFF_VALUE);
            size_t len;
            const char *name = asnix_sym_name(sym, &len);
            Value name_val = asnix_make_string_value(name, len);
            syms[i] = sym;
            vals[i] = asnix_make_lazyapp2(f, name_val, orig);
        }
        return make_attrs_from_sorted(syms, vals, n);
    }

    void *desc = asnix_thunkmap_make_desc(f, src_entries);
    for (int64_t i = 0; i < n; i++) {
        syms[i] = *(const uint32_t *)(src_entries + i * ASNIX_ATTR_SIZE + ASNIX_ATTR_OFF_SYM);
        vals[i] = asnix_thunkmap_pack(desc, i);
    }
    Value out = make_attrs_from_sorted(syms, vals, n);
    asnix_thunkmap_set_out(desc, out);
    return out;
}

static Value prim_mapAttrs(const Value *args, int64_t argc) {
    (void)argc;
    static IrProgram *inner = NULL;
    if (!inner) inner = build_curry2_body(make_primop_value((void *)prim_mapAttrs_apply2, 2));
    return make_curried2(args[0], inner);
}

/* ---- table ---- */

const AsnixBuiltinDef ASNIX_ATTRS_BUILTINS[] = {
    { "attrNames", prim_attrNames, 1 },
    { "attrValues", prim_attrValues, 1 },
    { "hasAttr", prim_hasAttr, 1 },       /* true arity 2, curried -- see build_curry2_body's comment */
    { "getAttr", prim_getAttr, 1 },       /* ditto */
    { "mapAttrs", prim_mapAttrs, 1 },     /* ditto */
    { "removeAttrs", prim_removeAttrs, 1 }, /* ditto */
};

const size_t ASNIX_ATTRS_BUILTINS_COUNT = sizeof(ASNIX_ATTRS_BUILTINS) / sizeof(ASNIX_ATTRS_BUILTINS[0]);
