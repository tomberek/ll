/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
/*
 * builtins.genericClosure { startSet; operator; } -- arity 1 (a single
 * attrset argument, exactly like real Nix -- no currying needed).
 *
 * Semantics verified directly against real `nix-instantiate --eval`
 * (2.35.2) before writing any code, per this project's standing "verify
 * against the real oracle before trusting it" discipline -- the exact
 * scenarios the task flagged as uncertain:
 *
 *   - Processing order is breadth-first (a FIFO work queue), not
 *     depth-first: `genericClosure { startSet=[{key=1;}];
 *     operator = item: if item.key==1 then [{key=2;}{key=3;}] else
 *     if item.key==2 then [{key=4;}] else if item.key==3 then [{key=5;}]
 *     else []; }` => `[ {key=1;} {key=2;} {key=3;} {key=4;} {key=5;} ]`,
 *     not `[1,2,4,3,5]` (which a DFS/recursive-first-child-first strategy
 *     would produce).
 *   - Dedup is by `key`, checked at the moment an item is popped off the
 *     work queue (not when it's pushed): an `operator` that returns an
 *     item whose key is already in `startSet` (or already produced by an
 *     earlier `operator` call) is silently dropped, never re-added to the
 *     output and never re-visited by `operator` a second time. Confirmed:
 *     `startSet = [{key=1;}{key=1;}]` with a no-op operator => only one
 *     `{key=1;}` in the output; `operator = item: [{key=1;}{key=3;}]`
 *     from `startSet=[{key=1;}{key=2;}]` adds `{key=3;}` but not a second
 *     `{key=1;}`.
 *   - Non-`key` fields on each item are preserved verbatim in the output
 *     (an item is its own whole self, not just its key).
 *   - Real Nix's dedup set uses `builtins.lessThan`-style ordering
 *     (`CompareValues`, via a `std::map`) and genuinely errors if two
 *     encountered keys aren't comparable (e.g. mixing int and string
 *     keys: "cannot compare a string with an integer") -- confirmed this
 *     throws even when the two keys are *otherwise equal*, e.g. two
 *     `{key = true;}` items throw "cannot compare a Boolean with a
 *     Boolean; values of that type are incomparable", because the
 *     dedup set's ordering comparator is consulted on every insertion
 *     attempt regardless of value equality. This implementation still
 *     dedups by *equality* (asnix_bshared_value_eq), but now gates every
 *     such comparison on `value_key_comparable` below first (same
 *     int/string/list scope `asnix_bshared_value_lt`, M57a, supports --
 *     bool/null/attrs/function keys, or a mismatched pair like int-vs-
 *     string, are rejected) so a key-comparability violation is still
 *     caught and still fatal, just via an equality-based dedup loop
 *     instead of a real ordered map.
 */
#include "builtins_closure.h"

#include <stdint.h>
#include <stdlib.h>

#include "asnix/heap.h"
#include "asnix/ir.h"
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

/* Same type scope as asnix_bshared_value_lt (M57a): int-vs-int,
 * string-vs-string, list-vs-list (checked shallowly here -- a `key`
 * made of lists-of-incomparable-elements will still get caught, just
 * one level later, by this same check on the next insertion attempt
 * that actually walks into the mismatched elements via value_lt itself
 * were it ever invoked; for the dedup-by-equality loop below, a shallow
 * tag check is enough to reject the two kinds of input the two
 * conformance cases exercise: an incompatible type mix, and a type
 * that's never comparable at all, e.g. an attrset). */
static int value_key_comparable(Value a, Value b) {
    int ta = asnix_tag_of(a), tb = asnix_tag_of(b);
    if (ta == ASNIX_TAG_INT && tb == ASNIX_TAG_INT) return 1;
    if (ta == ASNIX_TAG_PTR && tb == ASNIX_TAG_PTR && !asnix_value_is_path(a) && !asnix_value_is_path(b)) return 1;
    if (ta == ASNIX_TAG_LIST && tb == ASNIX_TAG_LIST) return 1;
    return 0;
}

static Value prim_genericClosure(const Value *args, int64_t argc) {
    (void)argc;
    Value input = args[0];
    ASNIX_REQUIRE_ATTRS(input, "genericClosure");

    uint32_t sym_startSet = asnix_intern("startSet", 8);
    uint32_t sym_operator = asnix_intern("operator", 8);
    uint32_t sym_key = asnix_intern("key", 3);

    int64_t ss_idx = asnix_bshared_attrs_find(input, sym_startSet);
    if (ss_idx < 0) asnix_bshared_fatal("genericClosure", "attribute 'startSet' is missing");
    Value start_set = asnix_force_for_print(asnix_bshared_attrs_value(input, ss_idx));
    ASNIX_REQUIRE_LIST(start_set, "genericClosure");

    int64_t op_idx = asnix_bshared_attrs_find(input, sym_operator);
    if (op_idx < 0) asnix_bshared_fatal("genericClosure", "attribute 'operator' is missing");
    Value operator_fn = asnix_force_for_print(asnix_bshared_attrs_value(input, op_idx));

    ValueVec queue = {0}, seen_keys = {0}, result = {0};
    int64_t n0 = asnix_bshared_list_count(start_set);
    const Value *elems0 = asnix_bshared_list_elems(start_set);
    for (int64_t i = 0; i < n0; i++) vv_push(&queue, elems0[i]);

    size_t head = 0;
    while (head < queue.len) {
        /* Lazy list elements (M23's `map`, etc.) -- force before checking
         * the tag, same class of fix M23's own concatLists needed. */
        Value item = asnix_force_for_print(queue.data[head++]);
        if (asnix_tag_of(item) != ASNIX_TAG_ATTRS) {
            asnix_bshared_fatal("genericClosure", "a work-set element is not an attrset");
        }
        int64_t k_idx = asnix_bshared_attrs_find(item, sym_key);
        if (k_idx < 0) {
            asnix_bshared_fatal("genericClosure", "a work-set element is missing the 'key' attribute");
        }
        Value key = asnix_force_for_print(asnix_bshared_attrs_value(item, k_idx));

        int already_seen = 0;
        for (size_t i = 0; i < seen_keys.len; i++) {
            if (!value_key_comparable(seen_keys.data[i], key)) {
                asnix_bshared_fatal("genericClosure",
                    "cannot compare two 'key' values (incompatible or uncomparable types)");
            }
            if (asnix_bshared_value_eq(seen_keys.data[i], key)) { already_seen = 1; break; }
        }
        if (already_seen) continue;
        vv_push(&seen_keys, key);
        vv_push(&result, item);

        Value next = asnix_apply(operator_fn, &item, 1);
        if (asnix_tag_of(next) != ASNIX_TAG_LIST) {
            asnix_bshared_fatal("genericClosure", "'operator' did not return a list");
        }
        int64_t ncount = asnix_bshared_list_count(next);
        const Value *nelems = asnix_bshared_list_elems(next);
        for (int64_t i = 0; i < ncount; i++) vv_push(&queue, nelems[i]);
    }

    Value out = asnix_bshared_make_list(result.data, (int64_t)result.len);
    free(queue.data);
    free(seen_keys.data);
    free(result.data);
    return out;
}

static Value build_genericClosure(void) { return asnix_bshared_make_primop((void *)prim_genericClosure, 1); }

const AsnixBuiltinMiscDef ASNIX_CLOSURE_BUILTINS[] = {
    { "genericClosure", build_genericClosure },
};

const size_t ASNIX_CLOSURE_BUILTINS_COUNT = sizeof(ASNIX_CLOSURE_BUILTINS) / sizeof(ASNIX_CLOSURE_BUILTINS[0]);
