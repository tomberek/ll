/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
/*
 * map, filter, foldl', elemAt, concatLists, tail, head. See
 * builtins_list.h for why these live in their own file/table.
 *
 * Currying: parser.c's parse_app always desugars `f x y` into nested
 * arity-1 AST_APP nodes, so OP_CALL only ever sees argc=1 calls for
 * builtin application. map/filter/foldl' need to carry an arbitrary
 * *runtime* Value (the mapped function, the predicate, the accumulator)
 * from one curried call into the next. The PrimOp heap object is
 * normally just {fn; arity} (16 bytes, heap.h) with no spare field and
 * no self/closure pointer handed to `fn`.
 *
 * The fix: allocate *larger* PrimOp objects for these curried stages --
 * {fn; arity; capture0; [capture1]}. Nothing anywhere checks that a
 * PrimOp heap object is exactly LL_PRIMOP_SIZE bytes, only that
 * offsets 0/8 hold fn/arity (see make_primop1/make_primop2 below). The
 * captured Value(s) are recovered inside the stage's own `fn` via
 * `self_captures()`, which relies on a real invariant of op_call.S's
 * primop call path: the callee's own tagged Value sits in the VALSTACK
 * slot immediately before `args[0]` (op_call.S computes `args = callee_
 * slot + 8` and never overwrites that slot before calling), so
 * `args[-1]` is always the exact PrimOp Value that was called -- letting
 * `fn` recover "self" and read its own trailing capture fields with no
 * assembly changes. This invariant holds for every *ordinary* Nix-level
 * application, which is the only way these stage-2/3 PrimOps are ever
 * invoked -- this module itself only ever uses ll_apply to call the
 * *user-supplied* function/predicate/operator, never its own curried
 * stages, so the ll_apply primop path (which does NOT preserve this
 * invariant -- it tail-calls `fn` with the caller's own args pointer,
 * nothing before it) never needs to recover a self pointer this way.
 * If you change op_call.S's primop calling convention, this invariant
 * (and self_captures()) must change with it.
 */
#include "builtins_list.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ll/arena.h"
#include "ll/heap.h"
#include "ll/ir.h"
#include "ll/value.h"
#include "builtins_shared.h"
#include "value_util.h" /* ll_make_lazyapp1 -- see prim_map_stage2's comment */

/* Plain, non-capturing PrimOp (LL_PRIMOP_SIZE bytes, heap.h). */
static Value make_primop0(void *fn, int64_t arity) {
    void *mem = ll_arena_alloc(LL_PRIMOP_SIZE);
    *(void **)((char *)mem + LL_PRIMOP_OFF_FN) = fn;
    *(int64_t *)((char *)mem + LL_PRIMOP_OFF_ARITY) = arity;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | LL_TAG_PRIMOP };
}

/* PrimOp with one trailing captured Value (see header comment above). */
static Value make_primop1(void *fn, int64_t arity, Value cap0) {
    void *mem = ll_arena_alloc(LL_PRIMOP_SIZE + sizeof(Value));
    *(void **)((char *)mem + LL_PRIMOP_OFF_FN) = fn;
    *(int64_t *)((char *)mem + LL_PRIMOP_OFF_ARITY) = arity;
    *(Value *)((char *)mem + LL_PRIMOP_SIZE) = cap0;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | LL_TAG_PRIMOP };
}

/* Same as make_primop1, but sets LL_PRIMOP_OFF_NOFORCE (heap.h) so
 * op_call.S/apply.S do NOT force this primop's own argument before
 * calling it. Used only for foldl''s stage2 -- see prim_foldl_prime's
 * own comment for why plain make_primop1 is wrong specifically here. */
static Value make_primop1_noforce(void *fn, int64_t arity, Value cap0) {
    void *mem = ll_arena_alloc(LL_PRIMOP_SIZE + sizeof(Value));
    *(void **)((char *)mem + LL_PRIMOP_OFF_FN) = fn;
    *(int64_t *)((char *)mem + LL_PRIMOP_OFF_ARITY) = arity;
    *(int64_t *)((char *)mem + LL_PRIMOP_OFF_NOFORCE) = 1;
    *(Value *)((char *)mem + LL_PRIMOP_SIZE) = cap0;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | LL_TAG_PRIMOP };
}

/* PrimOp with two trailing captured Values (foldl''s stage3: op + acc). */
static Value make_primop2(void *fn, int64_t arity, Value cap0, Value cap1) {
    void *mem = ll_arena_alloc(LL_PRIMOP_SIZE + 2 * sizeof(Value));
    *(void **)((char *)mem + LL_PRIMOP_OFF_FN) = fn;
    *(int64_t *)((char *)mem + LL_PRIMOP_OFF_ARITY) = arity;
    *(Value *)((char *)mem + LL_PRIMOP_SIZE) = cap0;
    *(Value *)((char *)mem + LL_PRIMOP_SIZE + sizeof(Value)) = cap1;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | LL_TAG_PRIMOP };
}

/* Recovers this call's own PrimOp heap pointer from the args pointer (see
 * header comment above for the op_call.S invariant this relies on), and
 * returns a pointer to its first trailing captured Value. */
static Value *self_captures(const Value *args) {
    Value self = args[-1];
    void *obj = (void *)(uintptr_t)(self.raw & ~(uint64_t)LL_TAG_MASK);
    return (Value *)((char *)obj + LL_PRIMOP_SIZE);
}

static void fatal(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

static void check_list(Value v, const char *who) {
    if (ll_tag_of(v) != LL_TAG_LIST) {
        fprintf(stderr, "ll: %s: expected a list\n", who);
        exit(1);
    }
}

static int64_t list_count(Value v) {
    const char *base = (const char *)(uintptr_t)(v.raw & ~(uint64_t)LL_TAG_MASK);
    return *(const int64_t *)(base + LL_LIST_OFF_COUNT);
}

static const Value *list_elems(Value v) {
    const char *base = (const char *)(uintptr_t)(v.raw & ~(uint64_t)LL_TAG_MASK);
    return (const Value *)(base + LL_LIST_OFF_ELEMS);
}

static Value make_list(int64_t count, const Value *src) {
    void *mem = ll_arena_alloc(8 + (uint64_t)count * sizeof(Value));
    *(int64_t *)((char *)mem + LL_LIST_OFF_COUNT) = count;
    if (count > 0) {
        memcpy((char *)mem + LL_LIST_OFF_ELEMS, src, (size_t)count * sizeof(Value));
    }
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | LL_TAG_LIST };
}

static Value apply2(Value fn, Value a, Value b) {
    Value step1 = ll_apply(fn, &a, 1);
    return ll_apply(step1, &b, 1);
}

/* --------------------------------------------------------------------- */
/* map f list                                                            */
/* --------------------------------------------------------------------- */

/* `map` must be LAZY per resulting element -- real Nix builds a list of
 * thunks, each calling `f elem` only once actually forced (verified:
 * `builtins.length (map (x: throw "boom") [1 2 3])` => 3, never
 * triggering the throw at all since nothing forces the elements here).
 * Do NOT call `ll_apply(f, &elem, 1)` eagerly while building the result
 * list here -- a prior version did exactly that and it is a real
 * strictness bug (confirmed via numtide/nix-conformance's
 * eval-okay-intersectAttrs.nix, which exposed the identical bug in
 * `mapAttrs`). Each element must stay a genuinely lazy LazyApp
 * (ll_make_lazyapp1, value_util.c) instead of an already-computed
 * value. */
static Value prim_map_stage2(const Value *args, int64_t argc) {
    (void)argc;
    Value f = self_captures(args)[0];
    Value list = args[0];
    check_list(list, "map");
    int64_t count = list_count(list);
    const Value *elems = list_elems(list);

    Value *out = count > 0 ? malloc((size_t)count * sizeof(Value)) : NULL;
    for (int64_t i = 0; i < count; i++) {
        out[i] = ll_make_lazyapp1(f, elems[i]);
    }
    Value result = make_list(count, out);
    free(out);
    return result;
}

static Value prim_map(const Value *args, int64_t argc) {
    (void)argc;
    return make_primop1((void *)prim_map_stage2, 1, args[0]);
}

Value ll_make_map_primop(void) {
    return make_primop0((void *)prim_map, 1);
}

/* --------------------------------------------------------------------- */
/* filter pred list                                                      */
/* --------------------------------------------------------------------- */

static Value prim_filter_stage2(const Value *args, int64_t argc) {
    (void)argc;
    Value pred = self_captures(args)[0];
    Value list = args[0];
    check_list(list, "filter");
    int64_t count = list_count(list);
    const Value *elems = list_elems(list);

    Value *kept = count > 0 ? malloc((size_t)count * sizeof(Value)) : NULL;
    int64_t n_kept = 0;
    for (int64_t i = 0; i < count; i++) {
        Value elem = elems[i];
        Value keep = ll_apply(pred, &elem, 1);
        if (ll_tag_of(keep) != LL_TAG_IMM ||
            (ll_imm_subtag(keep) != LL_IMM_TRUE && ll_imm_subtag(keep) != LL_IMM_FALSE)) {
            free(kept);
            fatal("ll: filter: predicate did not return a boolean");
        }
        if (ll_imm_subtag(keep) == LL_IMM_TRUE) {
            kept[n_kept++] = elem;
        }
    }
    Value result = make_list(n_kept, kept);
    free(kept);
    return result;
}

static Value prim_filter(const Value *args, int64_t argc) {
    (void)argc;
    return make_primop1((void *)prim_filter_stage2, 1, args[0]);
}

/* --------------------------------------------------------------------- */
/* foldl' op acc list                                                    */
/* --------------------------------------------------------------------- */

static Value prim_foldl_prime_stage3(const Value *args, int64_t argc) {
    (void)argc;
    Value *caps = self_captures(args);
    Value op = caps[0];
    Value acc = caps[1];
    Value list = args[0];
    check_list(list, "foldl'");
    int64_t count = list_count(list);
    const Value *elems = list_elems(list);

    for (int64_t i = 0; i < count; i++) {
        acc = apply2(op, acc, elems[i]);
    }
    return acc;
}

static Value prim_foldl_prime_stage2(const Value *args, int64_t argc) {
    (void)argc;
    Value op = self_captures(args)[0];
    Value acc = args[0];
    return make_primop2((void *)prim_foldl_prime_stage3, 1, op, acc);
}

static Value prim_foldl_prime(const Value *args, int64_t argc) {
    (void)argc;
    /* NOFORCE here -- the *next* call (supplying `acc`, handled by
     * prim_foldl_prime_stage2) must not force its argument. Real Nix's
     * own foldl' never forces the initial accumulator merely because it
     * was passed in: for a nonempty list it's threaded straight into
     * `op` as a raw value (only forced if `op`'s body actually reads
     * it). Confirmed against real nix-instantiate: `builtins.foldl'
     * (_: x: x) (throw "never forced") ["a" 42]` => 42, no throw.
     * Do NOT switch this to plain make_primop1 -- op_call.S's blanket
     * argument-forcing would then force `acc` the instant
     * `(foldl' op) acc` was called, before stage2's own body (which
     * only captures it, never inspects its tag) ever ran. stage3
     * (supplying `list`) deliberately keeps ordinary forcing -- it
     * genuinely needs the list's own LL_TAG_LIST tag/count/elems
     * (check_list). */
    return make_primop1_noforce((void *)prim_foldl_prime_stage2, 1, args[0]);
}

/* --------------------------------------------------------------------- */
/* elemAt list n                                                         */
/* --------------------------------------------------------------------- */

static Value prim_elemAt_stage2(const Value *args, int64_t argc) {
    (void)argc;
    Value list = self_captures(args)[0];
    Value idx_v = args[0];
    if (ll_tag_of(idx_v) != LL_TAG_INT) {
        fatal("ll: elemAt: index is not an integer");
    }
    int64_t idx = ll_int_value(idx_v);
    int64_t count = list_count(list);
    if (idx < 0 || idx >= count) {
        fprintf(stderr, "ll: elemAt: index %lld out of bounds (list has %lld elements)\n",
                (long long)idx, (long long)count);
        exit(1);
    }
    return list_elems(list)[idx];
}

static Value prim_elemAt(const Value *args, int64_t argc) {
    (void)argc;
    check_list(args[0], "elemAt");
    return make_primop1((void *)prim_elemAt_stage2, 1, args[0]);
}

/* --------------------------------------------------------------------- */
/* concatLists list-of-lists                                             */
/* --------------------------------------------------------------------- */

static Value prim_concatLists(const Value *args, int64_t argc) {
    (void)argc;
    Value outer = args[0];
    check_list(outer, "concatLists");
    int64_t ocount = list_count(outer);
    const Value *oelems_raw = list_elems(outer);

    /* Each outer element must be FORCED before checking its tag -- list
     * elements are always lazy by design, so an unforced Thunk here is
     * completely ordinary, not an error. Do NOT check `ll_tag_of()` on
     * the raw, possibly-still-thunked element directly: a prior version
     * did that and it broke once list elements became genuinely lazy
     * (`concatLists (map (x: map f strings) algos)` aborted with a
     * bogus "element N is not a list" instead of forcing the inner
     * map's still-lazy result first). */
    Value *oelems = ocount > 0 ? malloc((size_t)ocount * sizeof(Value)) : NULL;
    int64_t total = 0;
    for (int64_t i = 0; i < ocount; i++) {
        oelems[i] = ll_force_for_print(oelems_raw[i]);
        if (ll_tag_of(oelems[i]) != LL_TAG_LIST) {
            fprintf(stderr, "ll: concatLists: element %lld is not a list\n", (long long)i);
            exit(1);
        }
        total += list_count(oelems[i]);
    }

    void *mem = ll_arena_alloc(8 + (uint64_t)total * sizeof(Value));
    *(int64_t *)((char *)mem + LL_LIST_OFF_COUNT) = total;
    Value *out = (Value *)((char *)mem + LL_LIST_OFF_ELEMS);
    int64_t pos = 0;
    for (int64_t i = 0; i < ocount; i++) {
        int64_t icount = list_count(oelems[i]);
        if (icount > 0) {
            memcpy(out + pos, list_elems(oelems[i]), (size_t)icount * sizeof(Value));
        }
        pos += icount;
    }
    free(oelems);
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | LL_TAG_LIST };
}

/* --------------------------------------------------------------------- */
/* tail list / head list                                                 */
/* --------------------------------------------------------------------- */

static Value prim_tail(const Value *args, int64_t argc) {
    (void)argc;
    Value list = args[0];
    check_list(list, "tail");
    int64_t count = list_count(list);
    if (count == 0) {
        fatal("ll: tail: list is empty");
    }
    return make_list(count - 1, list_elems(list) + 1);
}

static Value prim_head(const Value *args, int64_t argc) {
    (void)argc;
    Value list = args[0];
    check_list(list, "head");
    if (list_count(list) == 0) {
        fatal("ll: head: list is empty");
    }
    return list_elems(list)[0];
}

/* --------------------------------------------------------------------- */

const LlBuiltinDef ll_builtins_list[] = {
    { "map", prim_map, 1 },
    { "filter", prim_filter, 1 },
    { "foldl'", prim_foldl_prime, 1 },
    { "elemAt", prim_elemAt, 1 },
    { "concatLists", prim_concatLists, 1 },
    { "tail", prim_tail, 1 },
    { "head", prim_head, 1 },
};

const size_t ll_builtins_list_count = sizeof(ll_builtins_list) / sizeof(ll_builtins_list[0]);
