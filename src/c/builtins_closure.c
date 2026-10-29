/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
/*
 * builtins.genericClosure { startSet; operator; } -- arity 1.
 *
 * Processing order is breadth-first (a FIFO work queue), not depth-first.
 * Dedup is by `key`, checked at the moment an item is popped off the work
 * queue (not when it's pushed): an `operator` that returns an item whose
 * key is already in `startSet` (or already produced by an earlier
 * `operator` call) is silently dropped, never re-added to the output and
 * never re-visited by `operator` a second time. Non-`key` fields on each
 * item are preserved verbatim in the output. Real Nix's dedup set also
 * errors if two encountered keys aren't comparable (e.g. mixing int and
 * string keys, or two booleans) even when otherwise equal -- this
 * implementation dedups by *equality* (ll_bshared_value_eq), but gates
 * every such comparison on `value_key_comparable` first so a
 * key-comparability violation is still caught and still fatal.
 */
#include "builtins_closure.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ll/heap.h"
#include "ll/ir.h"
#include "builtins_shared.h"
#include "symtab.h"
#include "value_util.h"

typedef struct {
    Value *data;
    size_t len, cap;
} ValueVec;

static void vv_push(ValueVec *v, Value x) {
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 16;
        v->data = realloc(v->data, v->cap * sizeof(Value));
    }
    v->data[v->len++] = x;
}

typedef enum { KC_INT, KC_STR, KC_LIST, KC_FLOAT, KC_OTHER } KeyClass;

/* Float is tagged LL_TAG_PTR like a string but must get its own
 * KeyClass: closure_key_hash's STR case hashes raw PTR bytes, and two
 * numerically-equal doubles are not always byte-identical (0.0 and -0.0
 * differ only in the sign bit) -- hashing them as opaque bytes would put
 * them in different buckets and the hash set would never call
 * ll_bshared_value_eq to notice they're equal, silently failing to
 * dedupe. Giving Float its own class with a hash that normalizes -0.0 to
 * +0.0 (below) closes that gap. */
static KeyClass key_class(Value v) {
    int t = ll_tag_of(v);
    if (t == LL_TAG_INT) return KC_INT;
    if (t == LL_TAG_PTR && ll_value_is_float(v)) return KC_FLOAT;
    if (t == LL_TAG_PTR && !ll_value_is_path(v)) return KC_STR;
    if (t == LL_TAG_LIST) return KC_LIST;
    return KC_OTHER;
}

/* Mirrors value_key_comparable's allowed pairs: both int, both string,
 * both list, both float -- KC_OTHER never matches anything, including
 * itself (so two `{key = true;}` items still throw). Note KC_INT and
 * KC_FLOAT are NOT mutually comparable here even though
 * ll_bshared_value_eq itself allows numeric int/float equality -- an int
 * key mixed with a float key must still throw "cannot compare". */
static int classes_comparable(KeyClass a, KeyClass b) {
    return (a == KC_INT && b == KC_INT) || (a == KC_STR && b == KC_STR) || (a == KC_LIST && b == KC_LIST) ||
           (a == KC_FLOAT && b == KC_FLOAT);
}

/* FNV-1a-style mix, folded recursively over a LIST key's elements (each
 * forced first). Two keys that are equal per ll_bshared_value_eq must
 * always hash equal here too -- the only correctness requirement a
 * hash-set dedup has. Only ever called on a key whose class is
 * KC_INT/KC_STR/KC_LIST/KC_FLOAT (KC_OTHER keys are never hashed -- see
 * prim_genericClosure below). */
static uint64_t closure_key_hash(Value v) {
    uint64_t h = 1469598103934665603ULL;
    int t = ll_tag_of(v);
    if (t == LL_TAG_INT) {
        h ^= (uint64_t)ll_int_value(v);
        h *= 0x100000001b3ULL;
        return h;
    }
    if (ll_value_is_float(v)) {
        /* Normalize -0.0 to +0.0 before hashing -- the one case where
         * two IEEE754 doubles compare equal (ll_bshared_value_eq's
         * own `da == db`) but have different bit patterns. Every other
         * finite value, and NaN (which is never equal to anything, even
         * itself, so a hash collision there is harmless -- value_eq
         * still correctly says "not equal" on probe), already hashes
         * consistently with its own equality notion without special
         * casing. */
        double d = ll_float_value(v);
        if (d == 0.0) d = 0.0; /* collapses -0.0 to +0.0; +0.0 is a no-op */
        uint64_t bits;
        memcpy(&bits, &d, sizeof(bits));
        h ^= bits;
        h *= 0x100000001b3ULL;
        return h;
    }
    if (t == LL_TAG_LIST) {
        int64_t n = ll_bshared_list_count(v);
        const Value *elems = ll_bshared_list_elems(v);
        h ^= (uint64_t)n;
        h *= 0x100000001b3ULL;
        for (int64_t i = 0; i < n; i++) {
            uint64_t eh = closure_key_hash(ll_force_for_print(elems[i]));
            h ^= eh;
            h *= 0x100000001b3ULL;
        }
        return h;
    }
    /* KC_STR (plain LL_TAG_PTR, non-path) -- same byte source
     * ll_bshared_value_eq's own PTR-vs-PTR case memcmp's. */
    int64_t len;
    const char *bytes = ll_ptr_bytes(v, &len);
    for (int64_t i = 0; i < len; i++) {
        h ^= (uint8_t)bytes[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

typedef struct {
    uint64_t hash;
    Value key;
    int used;
} ClosureSeenSlot;

typedef struct {
    ClosureSeenSlot *slots;
    size_t cap;   /* always a power of two */
    size_t count;
} ClosureSeenSet;

static void seen_set_init(ClosureSeenSet *s) {
    s->cap = 64;
    s->slots = calloc(s->cap, sizeof(ClosureSeenSlot));
    s->count = 0;
}

static void seen_set_insert_raw(ClosureSeenSet *s, uint64_t hash, Value key) {
    size_t mask = s->cap - 1;
    size_t i = (size_t)hash & mask;
    while (s->slots[i].used) i = (i + 1) & mask;
    s->slots[i].used = 1;
    s->slots[i].hash = hash;
    s->slots[i].key = key;
    s->count++;
}

static void seen_set_grow(ClosureSeenSet *s) {
    size_t old_cap = s->cap;
    ClosureSeenSlot *old = s->slots;
    s->cap = old_cap * 2;
    s->slots = calloc(s->cap, sizeof(ClosureSeenSlot));
    s->count = 0;
    for (size_t i = 0; i < old_cap; i++) {
        if (old[i].used) seen_set_insert_raw(s, old[i].hash, old[i].key);
    }
    free(old);
}

/* Returns 1 if an equal key is already present (per ll_bshared_value_eq);
 * otherwise inserts `key` and returns 0. Keeps the load factor <= 0.75. */
static int seen_set_probe_or_insert(ClosureSeenSet *s, Value key) {
    if (s->count * 4 >= s->cap * 3) seen_set_grow(s);
    uint64_t h = closure_key_hash(key);
    size_t mask = s->cap - 1;
    size_t i = (size_t)h & mask;
    while (s->slots[i].used) {
        if (s->slots[i].hash == h && ll_bshared_value_eq(s->slots[i].key, key)) return 1;
        i = (i + 1) & mask;
    }
    s->slots[i].used = 1;
    s->slots[i].hash = h;
    s->slots[i].key = key;
    s->count++;
    return 0;
}

static Value prim_genericClosure(const Value *args, int64_t argc) {
    (void)argc;
    Value input = args[0];
    LL_REQUIRE_ATTRS(input, "genericClosure");

    uint32_t sym_startSet = ll_intern("startSet", 8);
    uint32_t sym_operator = ll_intern("operator", 8);
    uint32_t sym_key = ll_intern("key", 3);

    int64_t ss_idx = ll_bshared_attrs_find(input, sym_startSet);
    if (ss_idx < 0) ll_bshared_fatal("genericClosure", "attribute 'startSet' is missing");
    Value start_set = ll_force_for_print(ll_bshared_attrs_value(input, ss_idx));
    LL_REQUIRE_LIST(start_set, "genericClosure");

    int64_t op_idx = ll_bshared_attrs_find(input, sym_operator);
    if (op_idx < 0) ll_bshared_fatal("genericClosure", "attribute 'operator' is missing");
    Value operator_fn = ll_force_for_print(ll_bshared_attrs_value(input, op_idx));

    ValueVec queue = {0}, result = {0};
    ClosureSeenSet seen;
    seen_set_init(&seen);
    int have_class = 0;
    KeyClass class0 = KC_OTHER;
    int64_t n0 = ll_bshared_list_count(start_set);
    const Value *elems0 = ll_bshared_list_elems(start_set);
    for (int64_t i = 0; i < n0; i++) vv_push(&queue, elems0[i]);

    size_t head = 0;
    while (head < queue.len) {
        Value item = ll_force_for_print(queue.data[head++]);
        if (ll_tag_of(item) != LL_TAG_ATTRS) {
            ll_bshared_fatal("genericClosure", "a work-set element is not an attrset");
        }
        int64_t k_idx = ll_bshared_attrs_find(item, sym_key);
        if (k_idx < 0) {
            ll_bshared_fatal("genericClosure", "a work-set element is missing the 'key' attribute");
        }
        Value key = ll_force_for_print(ll_bshared_attrs_value(item, k_idx));

        KeyClass kc = key_class(key);
        if (!have_class) {
            have_class = 1;
            class0 = kc;
        } else if (!classes_comparable(class0, kc)) {
            ll_bshared_fatal("genericClosure", "cannot compare two 'key' values (incompatible or uncomparable types)");
        }
        /* A KC_OTHER key is only ever reachable here on its very first
         * occurrence (any second key -- even another KC_OTHER one --
         * fails classes_comparable above and is already fatal by this
         * point), so it can never be "already seen"; skip the hash set
         * entirely for it (closure_key_hash/value_eq on KC_OTHER values
         * like attrsets/functions is unsupported and unnecessary). */
        int already_seen = (kc == KC_OTHER) ? 0 : seen_set_probe_or_insert(&seen, key);
        if (already_seen) continue;
        vv_push(&result, item);

        Value next = ll_apply(operator_fn, &item, 1);
        if (ll_tag_of(next) != LL_TAG_LIST) {
            ll_bshared_fatal("genericClosure", "'operator' did not return a list");
        }
        int64_t ncount = ll_bshared_list_count(next);
        const Value *nelems = ll_bshared_list_elems(next);
        for (int64_t i = 0; i < ncount; i++) vv_push(&queue, nelems[i]);
    }

    Value out = ll_bshared_make_list(result.data, (int64_t)result.len);
    free(queue.data);
    free(seen.slots);
    free(result.data);
    return out;
}

static Value build_genericClosure(void) { return ll_bshared_make_primop((void *)prim_genericClosure, 1); }

const LlBuiltinMiscDef LL_CLOSURE_BUILTINS[] = {
    { "genericClosure", build_genericClosure },
};

const size_t LL_CLOSURE_BUILTINS_COUNT = sizeof(LL_CLOSURE_BUILTINS) / sizeof(LL_CLOSURE_BUILTINS[0]);
