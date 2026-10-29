/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "builtins_attrs.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ll/arena.h"
#include "ll/env.h"
#include "ll/heap.h"
#include "ll/ir.h"
#include "ll/irbuild.h"
#include "builtins_shared.h"
#include "symtab.h"
#include "value_util.h"

/* ---- small shared helpers ---- */

static Value make_primop_value(void *fn, int64_t arity) {
    void *mem = ll_arena_alloc(LL_PRIMOP_SIZE);
    *(void **)((char *)mem + LL_PRIMOP_OFF_FN) = fn;
    *(int64_t *)((char *)mem + LL_PRIMOP_OFF_ARITY) = arity;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | LL_TAG_PRIMOP };
}

static void require_tag(Value v, int tag, const char *who, const char *what) {
    if (ll_tag_of(v) != tag) {
        fprintf(stderr, "ll: %s: argument is not %s\n", who, what);
        exit(1);
    }
}
#define require_attrs(v, who)  require_tag((v), LL_TAG_ATTRS, (who), "an attrset")
#define require_string(v, who) require_tag((v), LL_TAG_PTR, (who), "a string")
#define require_list(v, who)   require_tag((v), LL_TAG_LIST, (who), "a list")

/* `v` may be a lazy union header (heap.h) rather than a flat Attrs --
 * must resolve to the flat form first before reading raw layout. */
static int64_t attrs_count(Value v) {
    v = ll_bshared_attrs_resolve(v);
    return *(const int64_t *)((const char *)(v.raw & ~0x7ULL) + LL_ATTRS_OFF_COUNT);
}
static uint32_t attrs_entry_sym(Value v, int64_t i) {
    v = ll_bshared_attrs_resolve(v);
    const char *entry = (const char *)(v.raw & ~0x7ULL) + LL_ATTRS_OFF_ENTRIES + i * LL_ATTR_SIZE;
    return *(const uint32_t *)(entry + LL_ATTR_OFF_SYM);
}
static Value attrs_entry_value(Value v, int64_t i) {
    v = ll_bshared_attrs_resolve(v);
    const char *entry = (const char *)(v.raw & ~0x7ULL) + LL_ATTRS_OFF_ENTRIES + i * LL_ATTR_SIZE;
    return *(const Value *)(entry + LL_ATTR_OFF_VALUE);
}
static int64_t list_count(Value v) {
    return *(const int64_t *)((const char *)(v.raw & ~0x7ULL) + LL_LIST_OFF_COUNT);
}
static Value list_elem(Value v, int64_t i) {
    return *(const Value *)((const char *)(v.raw & ~0x7ULL) + LL_LIST_OFF_ELEMS + i * 8);
}
static Value make_list(const Value *elems, int64_t count) {
    void *mem = ll_arena_alloc(8 + (uint64_t)count * 8);
    *(int64_t *)((char *)mem + LL_LIST_OFF_COUNT) = count;
    if (count > 0) memcpy((char *)mem + LL_LIST_OFF_ELEMS, elems, (size_t)count * 8);
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | LL_TAG_LIST };
}
/* `syms`/`vals` must already be in ascending-sym order (same invariant
 * OP_SELECT's binary search and heap.h's "Attrs: ... sorted by sym" note
 * require). */
static Value make_attrs_from_sorted(const uint32_t *syms, const Value *vals, int64_t count) {
    void *mem = ll_arena_alloc(8 + (uint64_t)count * LL_ATTR_SIZE);
    *(int64_t *)((char *)mem + LL_ATTRS_OFF_COUNT) = count;
    for (int64_t i = 0; i < count; i++) {
        char *entry = (char *)mem + LL_ATTRS_OFF_ENTRIES + i * LL_ATTR_SIZE;
        /* Must be a 4-byte write -- entry's value bytes start right after sym (no pad),
         * so an 8-byte write here would clobber them. */
        *(uint32_t *)(entry + LL_ATTR_OFF_SYM) = syms[i];
        *(Value *)(entry + LL_ATTR_OFF_VALUE) = vals[i];
    }
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | LL_TAG_ATTRS };
}

static void *alloc_attrs_header(int64_t count) {
    void *mem = ll_arena_alloc(8 + (uint64_t)count * LL_ATTR_SIZE);
    *(int64_t *)((char *)mem + LL_ATTRS_OFF_COUNT) = count;
    return mem;
}
static void set_attrs_entry(void *mem, int64_t i, uint32_t sym, Value val) {
    char *entry = (char *)mem + LL_ATTRS_OFF_ENTRIES + i * LL_ATTR_SIZE;
    /* Must be a 4-byte write -- see make_attrs_from_sorted's identical note above. */
    *(uint32_t *)(entry + LL_ATTR_OFF_SYM) = sym;
    *(Value *)(entry + LL_ATTR_OFF_VALUE) = val;
}

static int64_t string_len(Value v) {
    return *(const int64_t *)((const char *)(v.raw & ~0x7ULL) + LL_STRING_OFF_LEN);
}
static const char *string_bytes(Value v) {
    return (const char *)(v.raw & ~0x7ULL) + LL_STRING_OFF_BYTES;
}

static int name_cmp(const char *an, size_t al, const char *bn, size_t bl) {
    size_t n = al < bl ? al : bl;
    int c = memcmp(an, bn, n);
    if (c != 0) return c;
    return (al > bl) - (al < bl);
}

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
 * alphabetical -- a real bug was once caused by assuming otherwise). */
static void sorted_name_order(Value attrs, int64_t n, int64_t *order) {
    NameOrderEntry *entries = malloc((size_t)n * sizeof(NameOrderEntry));
    for (int64_t i = 0; i < n; i++) {
        size_t len;
        const char *name = ll_sym_name(attrs_entry_sym(attrs, i), &len);
        entries[i] = (NameOrderEntry){ .idx = i, .name = name, .len = len };
    }
    qsort(entries, (size_t)n, sizeof(NameOrderEntry), name_order_entry_cmp);
    for (int64_t i = 0; i < n; i++) order[i] = entries[i].idx;
    free(entries);
}

static Value make_curried2(Value captured, IrProgram *inner_prog) {
    Env *env = ll_env_alloc(NULL, 1);
    env->values[0] = captured;
    void *mem = ll_arena_alloc(LL_CLOSURE_SIZE);
    *(void **)((char *)mem + LL_CLOSURE_OFF_UP) = env;
    *(IrProgram **)((char *)mem + LL_CLOSURE_OFF_PROG) = inner_prog;
    *(int64_t *)((char *)mem + LL_CLOSURE_OFF_ARITY) = 1;
    *(uint64_t *)((char *)mem + LL_CLOSURE_OFF_FORMALS) = ll_bshared_native_formals_marker().raw;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | LL_TAG_CLOSURE };
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
    int64_t *order = ll_arena_alloc((uint64_t)n * sizeof(int64_t));
    sorted_name_order(attrs, n, order);
    Value *out = ll_arena_alloc((uint64_t)n * sizeof(Value));
    for (int64_t i = 0; i < n; i++) {
        size_t len;
        const char *name = ll_sym_name(attrs_entry_sym(attrs, order[i]), &len);
        out[i] = ll_make_string_value(name, len);
    }
    return make_list(out, n);
}

static Value prim_attrValues(const Value *args, int64_t argc) {
    (void)argc;
    require_attrs(args[0], "attrValues");
    Value attrs = args[0];
    int64_t n = attrs_count(attrs);
    int64_t *order = ll_arena_alloc((uint64_t)n * sizeof(int64_t));
    sorted_name_order(attrs, n, order);
    Value *out = ll_arena_alloc((uint64_t)n * sizeof(Value));
    for (int64_t i = 0; i < n; i++) out[i] = attrs_entry_value(attrs, order[i]);
    return make_list(out, n);
}

/* ---- hasAttr ---- */

static Value prim_hasAttr_apply2(const Value *args, int64_t argc) {
    (void)argc;
    require_string(args[0], "hasAttr");
    require_attrs(args[1], "hasAttr");
    uint32_t sym = ll_intern(string_bytes(args[0]), (size_t)string_len(args[0]));
    Value unused;
    return ll_bshared_attrs_probe(args[1], sym, &unused) ? LL_TRUE : LL_FALSE;
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
    uint32_t sym = ll_intern(string_bytes(args[0]), (size_t)string_len(args[0]));
    Value val;
    if (!ll_bshared_attrs_probe(args[1], sym, &val)) {
        fprintf(stderr, "ll: getAttr: attribute '%.*s' missing\n",
                (int)string_len(args[0]), string_bytes(args[0]));
        exit(1);
    }
    return val;
}

static Value prim_getAttr(const Value *args, int64_t argc) {
    (void)argc;
    static IrProgram *inner = NULL;
    if (!inner) inner = build_curry2_body(make_primop_value((void *)prim_getAttr_apply2, 2));
    return make_curried2(args[0], inner);
}

/* ---- removeAttrs ---- */

#define LL_REMOVE_ATTRS_SORT_INSERTION_MAX 32

#define LL_REMOVE_ATTRS_PRODUCT_MAX 2048

static int cmp_u32(const void *a, const void *b) {
    uint32_t ua = *(const uint32_t *)a, ub = *(const uint32_t *)b;
    return (ua > ub) - (ua < ub);
}

static int remove_syms_contains(const uint32_t *sorted, int64_t nnames, uint32_t sym) {
    int64_t lo = 0, hi = nnames - 1;
    while (lo <= hi) {
        int64_t mid = lo + (hi - lo) / 2;
        if (sorted[mid] == sym) return 1;
        if (sorted[mid] < sym) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0;
}

static Value prim_removeAttrs_apply2(const Value *args, int64_t argc) {
    (void)argc;
    require_attrs(args[0], "removeAttrs");
    require_list(args[1], "removeAttrs");
    Value attrs = args[0];
    Value namelist = args[1];
    int64_t nnames = list_count(namelist);
    uint32_t *remove_syms = ll_arena_alloc((uint64_t)nnames * sizeof(uint32_t));
    for (int64_t i = 0; i < nnames; i++) {
        /* `namelist`'s elements may be unforced Thunks -- force before
         * reading the raw tag/bytes. */
        Value nv = ll_force_for_print(list_elem(namelist, i));
        require_string(nv, "removeAttrs");
        remove_syms[i] = ll_intern(string_bytes(nv), (size_t)string_len(nv));
    }
    int64_t n = attrs_count(attrs);
    int use_sorted = n * nnames > LL_REMOVE_ATTRS_PRODUCT_MAX;
    if (use_sorted) {
        if (nnames <= LL_REMOVE_ATTRS_SORT_INSERTION_MAX) {
            for (int64_t i = 1; i < nnames; i++) {
                uint32_t e = remove_syms[i];
                int64_t j = i;
                while (j > 0 && remove_syms[j - 1] > e) {
                    remove_syms[j] = remove_syms[j - 1];
                    j--;
                }
                remove_syms[j] = e;
            }
        } else {
            qsort(remove_syms, (size_t)nnames, sizeof(uint32_t), cmp_u32);
        }
    }
    void *mem = alloc_attrs_header(n);
    int64_t out_n = 0;
    for (int64_t i = 0; i < n; i++) {
        uint32_t sym = attrs_entry_sym(attrs, i);
        int removed;
        if (use_sorted) {
            removed = remove_syms_contains(remove_syms, nnames, sym);
        } else {
            removed = 0;
            for (int64_t j = 0; j < nnames; j++) {
                if (remove_syms[j] == sym) { removed = 1; break; }
            }
        }
        if (!removed) {
            /* out entries inherit `attrs`'s own ascending-sym order since
             * we only filter in a single forward pass, never reorder. */
            set_attrs_entry(mem, out_n, sym, attrs_entry_value(attrs, i));
            out_n++;
        }
    }
    *(int64_t *)((char *)mem + LL_ATTRS_OFF_COUNT) = out_n;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | LL_TAG_ATTRS };
}

static Value prim_removeAttrs(const Value *args, int64_t argc) {
    (void)argc;
    static IrProgram *inner = NULL;
    if (!inner) inner = build_curry2_body(make_primop_value((void *)prim_removeAttrs_apply2, 2));
    return make_curried2(args[0], inner);
}

Value ll_make_removeAttrs_primop(void) {
    return make_primop_value((void *)prim_removeAttrs, 1);
}

/* ---- mapAttrs ---- */

/* `flat`'s entries array is reused directly as the descriptor's
 * `src_entries` (no copy): `out`'s syms are copied straight across from
 * `flat`'s in the same order below, so `out`'s index `i` and `flat`'s
 * index `i` always name the same key. */
static Value prim_mapAttrs_apply2(const Value *args, int64_t argc) {
    (void)argc;
    Value f = args[0];
    Value attrs = args[1];
    require_attrs(attrs, "mapAttrs");
    /* `src_entries` below is only valid once `attrs` is resolved to flat form. */
    Value flat = ll_bshared_attrs_resolve(attrs);
    const char *flat_base = (const char *)(flat.raw & ~0x7ULL);
    int64_t n = *(const int64_t *)(flat_base + LL_ATTRS_OFF_COUNT);
    const char *src_entries = flat_base + LL_ATTRS_OFF_ENTRIES;
    if (n == 0) return make_attrs_from_sorted(NULL, NULL, 0);

    void *mem = alloc_attrs_header(n);

    if (n > LL_THUNKMAP_MAX_IDX + 1) {
        /* More entries than a packed ThunkMap index (16 bits) can
         * address -- fall back to the per-element LazyApp path rather
         * than silently truncating an index (see heap.h's
         * LL_THUNKMAP_MARKER comment). */
        for (int64_t i = 0; i < n; i++) {
            const char *entry = src_entries + i * LL_ATTR_SIZE;
            uint32_t sym = *(const uint32_t *)(entry + LL_ATTR_OFF_SYM);
            Value orig = *(const Value *)(entry + LL_ATTR_OFF_VALUE);
            size_t len;
            const char *name = ll_sym_name(sym, &len);
            Value name_val = ll_make_string_value(name, len);
            set_attrs_entry(mem, i, sym, ll_make_lazyapp2(f, name_val, orig));
        }
        return (Value){ .raw = (uint64_t)(uintptr_t)mem | LL_TAG_ATTRS };
    }

    void *desc = ll_thunkmap_make_desc(f, src_entries);
    for (int64_t i = 0; i < n; i++) {
        uint32_t sym = *(const uint32_t *)(src_entries + i * LL_ATTR_SIZE + LL_ATTR_OFF_SYM);
        set_attrs_entry(mem, i, sym, ll_thunkmap_pack(desc, i));
    }
    Value out = (Value){ .raw = (uint64_t)(uintptr_t)mem | LL_TAG_ATTRS };
    ll_thunkmap_set_out(desc, out);
    return out;
}

static Value prim_mapAttrs(const Value *args, int64_t argc) {
    (void)argc;
    static IrProgram *inner = NULL;
    if (!inner) inner = build_curry2_body(make_primop_value((void *)prim_mapAttrs_apply2, 2));
    return make_curried2(args[0], inner);
}

/* ---- table ---- */

const LlBuiltinDef LL_ATTRS_BUILTINS[] = {
    { "attrNames", prim_attrNames, 1 },
    { "attrValues", prim_attrValues, 1 },
    { "hasAttr", prim_hasAttr, 1 },       /* true arity 2, curried */
    { "getAttr", prim_getAttr, 1 },       /* ditto */
    { "mapAttrs", prim_mapAttrs, 1 },     /* ditto */
    { "removeAttrs", prim_removeAttrs, 1 }, /* ditto */
};

const size_t LL_ATTRS_BUILTINS_COUNT = sizeof(LL_ATTRS_BUILTINS) / sizeof(LL_ATTRS_BUILTINS[0]);
