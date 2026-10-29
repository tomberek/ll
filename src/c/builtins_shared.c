/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "builtins_shared.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ll/arena.h"
#include "ll/env.h"
#include "ll/heap.h"
#include "ll/stats.h"
#include "ll/ir.h"
#include "ll/irbuild.h"
#include "symtab.h"
#include "value_util.h"

Value ll_bshared_make_primop(void *fn, int64_t arity) {
    void *mem = ll_arena_alloc(LL_PRIMOP_SIZE);
    ll_stats_primop++;
    *(void **)((char *)mem + LL_PRIMOP_OFF_FN) = fn;
    *(int64_t *)((char *)mem + LL_PRIMOP_OFF_ARITY) = arity;
    /* LL_PRIMOP_OFF_NOFORCE left at its fresh-arena zero (force args is
     * the default for every primop except tryEval). */
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | LL_TAG_PRIMOP };
}

Value ll_bshared_make_primop_noforce(void *fn, int64_t arity) {
    void *mem = ll_arena_alloc(LL_PRIMOP_SIZE);
    ll_stats_primop++;
    *(void **)((char *)mem + LL_PRIMOP_OFF_FN) = fn;
    *(int64_t *)((char *)mem + LL_PRIMOP_OFF_ARITY) = arity;
    *(int64_t *)((char *)mem + LL_PRIMOP_OFF_NOFORCE) = 1;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | LL_TAG_PRIMOP };
}

Value ll_bshared_native_formals_marker(void) {
    static Value marker = { .raw = 0 };
    if (marker.raw == 0) marker = ll_bshared_make_attrs(NULL, NULL, 0);
    return marker;
}

Value ll_bshared_make_curried_native(void *native_fn, int64_t arity) {
    Value native_primop = ll_bshared_make_primop(native_fn, arity);
    Value empty_formals = ll_bshared_native_formals_marker();

    IrBuilder *b = ib_new();
    ib_push_const(b, (int64_t)native_primop.raw);
    for (int64_t depth = arity - 1; depth >= 0; depth--) ib_var_ref(b, depth, 0);
    ib_call(b, arity);
    ib_ret(b);
    IrProgram *prog = ib_finish(b);

    for (int64_t i = arity - 1; i >= 1; i--) {
        IrBuilder *wrap_b = ib_new();
        ib_lambda(wrap_b, 1, prog, (int64_t)empty_formals.raw);
        ib_ret(wrap_b);
        prog = ib_finish(wrap_b);
    }

    void *mem = ll_arena_alloc(LL_CLOSURE_SIZE);
    ll_stats_closure++;
    *(void **)((char *)mem + LL_CLOSURE_OFF_UP) = NULL;
    *(IrProgram **)((char *)mem + LL_CLOSURE_OFF_PROG) = prog; /* one pointer, not {ops,pool} */
    *(int64_t *)((char *)mem + LL_CLOSURE_OFF_ARITY) = 1;
    *(uint64_t *)((char *)mem + LL_CLOSURE_OFF_FORMALS) = empty_formals.raw;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | LL_TAG_CLOSURE };
}

Value ll_bshared_make_curried_native_noforce(void *native_fn, int64_t arity) {
    ll_stats_closure++;
    Value native_primop = ll_bshared_make_primop_noforce(native_fn, arity);
    Value empty_formals = ll_bshared_native_formals_marker();

    IrBuilder *b = ib_new();
    ib_push_const(b, (int64_t)native_primop.raw);
    for (int64_t depth = arity - 1; depth >= 0; depth--) ib_var_ref_raw(b, depth, 0);
    ib_call(b, arity);
    ib_ret(b);
    IrProgram *prog = ib_finish(b);

    for (int64_t i = arity - 1; i >= 1; i--) {
        IrBuilder *wrap_b = ib_new();
        ib_lambda(wrap_b, 1, prog, (int64_t)empty_formals.raw);
        ib_ret(wrap_b);
        prog = ib_finish(wrap_b);
    }

    void *mem = ll_arena_alloc(LL_CLOSURE_SIZE);
    *(void **)((char *)mem + LL_CLOSURE_OFF_UP) = NULL;
    *(IrProgram **)((char *)mem + LL_CLOSURE_OFF_PROG) = prog; /* one pointer, not {ops,pool} */
    *(int64_t *)((char *)mem + LL_CLOSURE_OFF_ARITY) = 1;
    *(uint64_t *)((char *)mem + LL_CLOSURE_OFF_FORMALS) = empty_formals.raw;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | LL_TAG_CLOSURE };
}

Value ll_bshared_apply2(Value fn, Value a, Value b) {
    Value step1 = ll_apply(fn, &a, 1);
    return ll_apply(step1, &b, 1);
}

void ll_bshared_fatal(const char *who, const char *msg) {
    fprintf(stderr, "ll: %s: %s\n", who, msg);
    exit(1);
}

/* OP_SELECT's (op_select.S) static-attribute-missing fatal path -- names
 * the attribute, matching real Nix's own "attribute 'x' missing" wording. */
_Noreturn void ll_bshared_select_missing_fatal(uint32_t sym) {
    size_t len;
    const char *name = ll_sym_name(sym, &len);
    fprintf(stderr, "ll: attribute '%.*s' missing, aborting\n", (int)len, name);
    exit(1);
}

/* Deliberately does NOT force `v` first: every call site below reads a
 * value already sitting on VALSTACK or passed in from an asm op's own C
 * glue, which by this evaluator's standing invariant (every value reaching
 * VALSTACK is already forced) is never an unforced Thunk in practice --
 * the LL_TAG_THUNK case is dead code, kept only so a future violation of
 * that invariant prints something sane instead of misreading raw thunk
 * bits. */
const char *ll_bshared_type_name(Value v) {
    switch (ll_tag_of(v)) {
        case LL_TAG_INT: return "an integer";
        case LL_TAG_IMM:
            switch (ll_imm_subtag(v)) {
                case LL_IMM_TRUE:
                case LL_IMM_FALSE: return "a boolean";
                case LL_IMM_NULL: return "null";
                default: return "an immediate value";
            }
        case LL_TAG_PTR:
            return ll_value_is_path(v) ? "a path" : ll_value_is_float(v) ? "a float" : "a string";
        case LL_TAG_LIST: return "a list";
        case LL_TAG_ATTRS: return "an attrset";
        case LL_TAG_CLOSURE:
        case LL_TAG_PRIMOP: return "a function";
        case LL_TAG_THUNK: return "an unforced thunk"; /* dead in practice, see comment above */
        default: return "a value of unknown type";
    }
}

/* `v` arrives still tagged (the raw Value popped off VALSTACK, before
 * op_select.S's own `andq $-8` untagging) -- exactly what
 * ll_bshared_type_name above wants. */
_Noreturn void ll_bshared_select_type_fatal(Value v) {
    fprintf(stderr, "ll: '.' selection target is not an attrset (got %s), aborting\n",
            ll_bshared_type_name(v));
    exit(1);
}

/* `v` arrives still tagged (read out of the callee's VALSTACK slot
 * before op_call.S's own closure/primop dispatch, which is exactly the
 * point this path is reached from -- neither tag matched). */
_Noreturn void ll_bshared_call_type_fatal(Value v) {
    fprintf(stderr, "ll: call target is not a function (got %s), aborting\n",
            ll_bshared_type_name(v));
    exit(1);
}

/* `__functor` protocol: calling an attrset `s` with argument `arg`
 * (`s arg`) is sugar for `s.__functor s arg` when `s` has a `__functor`
 * attribute -- real Nix's exact mechanism: `__functor` is called with
 * the attrset itself as its first argument, then the real argument
 * second (a curried 2-arg function in Nix terms, represented here as two
 * ordinary `ll_apply` calls chained). Reached from both `OP_CALL`
 * (op_call.S) and `ll_apply` (apply.S) ONLY after their own Closure/
 * PrimOp dispatch has already failed to match -- so this never costs
 * anything on the common (plain function call) path. Does not special-
 * case a `__functor` that itself returns another attrset-with-
 * `__functor`; `ll_apply`'s own ATTRS branch (which this function's
 * second `ll_apply` call re-enters) already handles that recursively
 * for free. Fatal (same "not a function" message as a plain attrset) if
 * `attrs_val` has no `__functor` attribute at all. */
Value ll_bshared_call_functor(Value attrs_val, Value arg) {
    uint32_t sym = ll_intern("__functor", 9);
    int64_t idx = ll_bshared_attrs_find(attrs_val, sym);
    if (idx < 0) ll_bshared_call_type_fatal(attrs_val);
    Value functor_fn = ll_force_for_print(ll_bshared_attrs_value(attrs_val, idx));
    Value step1 = ll_apply(functor_fn, &attrs_val, 1);
    return ll_apply(step1, &arg, 1);
}

/* OP_CHECK_FORMALS's (op_check_formals.S) two lambda-pattern-mismatch
 * fatal paths. `sym` arrives already resolved out of the IR constant pool
 * (missing case, op_check_formals.S's %r15) or out of the given attrset's
 * own entries (extra case, %r11) -- both already-interned symbol ids. */
_Noreturn void ll_bshared_cf_missing_fatal(uint32_t sym) {
    size_t len;
    const char *name = ll_sym_name(sym, &len);
    fprintf(stderr, "ll: lambda called without a required argument '%.*s', aborting\n", (int)len, name);
    exit(1);
}

_Noreturn void ll_bshared_cf_extra_fatal(uint32_t sym) {
    size_t len;
    const char *name = ll_sym_name(sym, &len);
    fprintf(stderr, "ll: lambda called with an unexpected argument '%.*s', aborting\n", (int)len, name);
    exit(1);
}

void ll_bshared_require_tag(Value v, int tag, const char *who, const char *what) {
    if (ll_tag_of(v) != tag) {
        fprintf(stderr, "ll: %s: argument is not %s\n", who, what);
        exit(1);
    }
}

int64_t ll_bshared_list_count(Value v) {
    return *(const int64_t *)((const char *)(uintptr_t)(v.raw & ~(uint64_t)LL_TAG_MASK) + LL_LIST_OFF_COUNT);
}

const Value *ll_bshared_list_elems(Value v) {
    return (const Value *)((const char *)(uintptr_t)(v.raw & ~(uint64_t)LL_TAG_MASK) + LL_LIST_OFF_ELEMS);
}

Value ll_bshared_make_list(const Value *elems, int64_t count) {
    void *mem = ll_arena_alloc(8 + (uint64_t)count * sizeof(Value));
    ll_stats_list++;
    ll_stats_list_elements += (uint64_t)count;
    *(int64_t *)((char *)mem + LL_LIST_OFF_COUNT) = count;
    if (count > 0) memcpy((char *)mem + LL_LIST_OFF_ELEMS, elems, (size_t)count * sizeof(Value));
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | LL_TAG_LIST };
}

/* Merges two already-flat (sym-sorted) attrsets' entry arrays into a
 * caller-supplied output buffer, preserving `//`'s right-biased collision
 * semantics (rhs wins on a duplicate key). Same two-pointer merge logic
 * as ll_attrs_eager_merge's own inner loop (attrs_update.c), duplicated
 * here because this helper writes into a buffer the CALLER already owns
 * instead of allocating its own. Used ONLY by ll_bshared_attrs_resolve's
 * internal merge rounds below. Returns the number of entries written
 * (<= acount+bcount, shrinks only on duplicate keys); `out_entries` must
 * have room for at least acount+bcount entries. */
static int64_t attrs_merge_entries_into(const char *aentries, int64_t acount, const char *bentries, int64_t bcount,
                                         char *out_entries) __attribute__((noinline));
static int64_t attrs_merge_entries_into(const char *aentries, int64_t acount, const char *bentries, int64_t bcount,
                                         char *out_entries) {
    const char *pa = aentries;
    const char *pb = bentries;
    const char *aend = aentries + acount * LL_ATTR_SIZE;
    const char *bend = bentries + bcount * LL_ATTR_SIZE;
    char *pout = out_entries;
    while (pa < aend && pb < bend) {
        uint32_t asym = *(const uint32_t *)(pa + LL_ATTR_OFF_SYM);
        uint32_t bsym = *(const uint32_t *)(pb + LL_ATTR_OFF_SYM);
        if (asym < bsym) {
            memcpy(pout, pa, LL_ATTR_SIZE);
            pa += LL_ATTR_SIZE;
        } else if (bsym < asym) {
            memcpy(pout, pb, LL_ATTR_SIZE);
            pb += LL_ATTR_SIZE;
        } else {
            /* duplicate key: rhs (b) wins, matching `//`'s right-biased semantics */
            memcpy(pout, pb, LL_ATTR_SIZE);
            pa += LL_ATTR_SIZE;
            pb += LL_ATTR_SIZE;
        }
        pout += LL_ATTR_SIZE;
    }
    while (pa < aend) {
        memcpy(pout, pa, LL_ATTR_SIZE);
        pa += LL_ATTR_SIZE;
        pout += LL_ATTR_SIZE;
    }
    while (pb < bend) {
        memcpy(pout, pb, LL_ATTR_SIZE);
        pb += LL_ATTR_SIZE;
        pout += LL_ATTR_SIZE;
    }
    return (pout - out_entries) / LL_ATTR_SIZE;
}

/* See builtins_shared.h's comment and heap.h's UnionHdr layout. */
Value ll_bshared_attrs_resolve(Value v) {
    const char *base = (const char *)(uintptr_t)(v.raw & ~(uint64_t)LL_TAG_MASK);
    int64_t count = *(const int64_t *)(base + LL_ATTRS_OFF_COUNT);
    if (count >= 0) return v; /* already flat -- the common case */

    Value cached = *(const Value *)(base + LL_ATTRS_UNION_OFF_FLATCACHE);
    if (cached.raw != 0) return cached; /* materialized by an earlier call */

    int64_t nleaves = -count;
    const Value *leaves = (const Value *)(base + LL_ATTRS_UNION_OFF_LEAVES);

    if (nleaves == 1) {
        /* No merge at all needed -- leaves[0] is already a real,
         * permanent, flat attrs object (guaranteed flat by construction,
         * see the comment below), so there's nothing to copy. */
        Value acc = leaves[0];
        *(Value *)((char *)base + LL_ATTRS_UNION_OFF_FLATCACHE) = acc;
        return acc;
    }

    /* Leaves are guaranteed flat by construction (ll_attrs_update,
     * attrs_update.c, always resolves a would-be leaf before storing it).
     *
     * This is a bottom-up PAIRWISE (binary-tree) merge, NOT a
     * left-to-right fold (`acc = merge(acc, leaf)` repeated) -- a fold
     * would re-copy the ever-growing accumulator on every merge step,
     * costing O(total_size * nleaves) instead of O(total_size *
     * log2(nleaves)). The pairwise merge below halves the work list each
     * round (same shape as mergesort's merge phase), preserving `//`'s
     * right-biased collision semantics: each pairwise merge of
     * (entries_ptr[i], entries_cnt[i]) with (entries_ptr[i+1],
     * entries_cnt[i+1]) keeps slot i+1 (the more-recently-layered,
     * higher-priority side, since `leaves` is stored lowest-priority-
     * first) as the winning operand on a duplicate key, and pairing
     * adjacent elements bottom-up preserves that same relative priority
     * order all the way up the tree.
     *
     * Every round EXCEPT the last writes into one of a handful of plain
     * `malloc` scratch buffers instead of the permanent arena. NOT a
     * ping-pong between just 2 buffers -- a carried-over, unmerged
     * odd-one-out entry from an earlier round can end up pointing into a
     * buffer a LATER round would then overwrite -- one DEDICATED buffer
     * per round, never reused across rounds. LL_ATTRS_UNION_MAX_LEAVES
     * (64) bounds the round count at log2(64)=6, so at most 6 scratch
     * buffers are ever allocated, each sized to `upper_bound` entries
     * (the total entry count summed across all original leaves, which
     * bounds every round's output since a pairwise merge only ever
     * shrinks the total via duplicate-key collisions).
     *
     * The LAST round is always a plain 2-way merge (the loop below only
     * runs while n > 2, and a round's output count is ceil(n/2), which
     * only reaches 1 by going through exactly n == 2) and writes DIRECTLY
     * into one real, permanent arena allocation instead of one more
     * scratch buffer plus a copy. Every scratch buffer (if any were
     * allocated at all) is freed right after -- none of them outlive
     * this function call. */
    const char *entries_ptr[LL_ATTRS_UNION_MAX_LEAVES];
    int64_t entries_cnt[LL_ATTRS_UNION_MAX_LEAVES];
    int64_t upper_bound = 0;
    for (int64_t i = 0; i < nleaves; i++) {
        const char *lb = (const char *)(uintptr_t)(leaves[i].raw & ~(uint64_t)LL_TAG_MASK);
        int64_t lc = *(const int64_t *)(lb + LL_ATTRS_OFF_COUNT);
        entries_ptr[i] = lb + LL_ATTRS_OFF_ENTRIES;
        entries_cnt[i] = lc;
        upper_bound += lc;
    }

    void *scratch[6]; /* log2(LL_ATTRS_UNION_MAX_LEAVES) == 6 */
    int64_t n_scratch = 0;
    size_t scratch_bytes = (size_t)upper_bound * LL_ATTR_SIZE;
    if (scratch_bytes == 0) scratch_bytes = 1; /* malloc(0) is implementation-defined; avoid relying on it */

    int64_t n = nleaves;

    /* Special case: when leaves[0] (the oldest/lowest-priority leaf, since
     * ll_attrs_update's make_union_header always appends the NEW leaf
     * after the EXISTING ones) alone outweighs every other leaf combined,
     * merge the small leaves together first (cheap, since they're small)
     * into one combined leaf using the same bottom-up scratch-buffer round
     * logic as the main loop below, then let the UNCHANGED n==2 tail
     * merge handle (leaf0, combined-small) as its final, direct-to-arena
     * merge -- O(big_size) paid once instead of O(big_size *
     * log2(nleaves)). Correctness: restricted to leaves[0] specifically
     * (not "whichever leaf happens to be biggest") because that's the
     * only position construction guarantees is safe --
     * attrs_merge_entries_into's right-biased collision rule (rhs always
     * wins) needs the big leaf on the lhs and everything newer-and-
     * combined on the rhs, which is only correct when the big leaf is the
     * OLDEST (lowest-priority) one, i.e. leaves[0]. The small-leaves-merge
     * below preserves their own relative priority order (newest-of-the-
     * rest wins) exactly as the unmodified main loop would. Setting `n =
     * 2` here and leaving entries_ptr[0]/entries_cnt[0] untouched makes
     * the `while (n > 2)` loop below a no-op, falling straight through to
     * the existing n==2 tail merge with no further change needed there. */
    if (nleaves >= 3) {
        int64_t rest_count = upper_bound - entries_cnt[0];
        if (entries_cnt[0] > rest_count) {
            const char *rptr[LL_ATTRS_UNION_MAX_LEAVES];
            int64_t rcnt[LL_ATTRS_UNION_MAX_LEAVES];
            for (int64_t i = 1; i < nleaves; i++) {
                rptr[i - 1] = entries_ptr[i];
                rcnt[i - 1] = entries_cnt[i];
            }
            int64_t rn = nleaves - 1;
            while (rn > 1) {
                void *mem = malloc(scratch_bytes);
                if (!mem) {
                    fprintf(stderr, "ll: out of memory\n");
                    exit(1);
                }
                scratch[n_scratch++] = mem;
                char *cursor = (char *)mem;
                int64_t out_n = 0;
                int64_t i = 0;
                for (; i + 1 < rn; i += 2) {
                    int64_t k = attrs_merge_entries_into(rptr[i], rcnt[i], rptr[i + 1], rcnt[i + 1], cursor);
                    rptr[out_n] = cursor;
                    rcnt[out_n] = k;
                    out_n++;
                    cursor += k * LL_ATTR_SIZE;
                }
                if (i < rn) {
                    rptr[out_n] = rptr[i];
                    rcnt[out_n] = rcnt[i];
                    out_n++;
                }
                rn = out_n;
            }
            entries_ptr[1] = rptr[0];
            entries_cnt[1] = rcnt[0];
            n = 2;
        }
    }

    while (n > 2) {
        void *mem = malloc(scratch_bytes);
        if (!mem) {
            fprintf(stderr, "ll: out of memory\n");
            exit(1);
        }
        scratch[n_scratch++] = mem;
        char *cursor = (char *)mem;

        int64_t out_n = 0;
        int64_t i = 0;
        for (; i + 1 < n; i += 2) {
            int64_t k = attrs_merge_entries_into(entries_ptr[i], entries_cnt[i], entries_ptr[i + 1],
                                                  entries_cnt[i + 1], cursor);
            entries_ptr[out_n] = cursor;
            entries_cnt[out_n] = k;
            out_n++;
            cursor += k * LL_ATTR_SIZE;
        }
        if (i < n) { /* odd one out, carried up unmerged */
            entries_ptr[out_n] = entries_ptr[i];
            entries_cnt[out_n] = entries_cnt[i];
            out_n++;
        }
        n = out_n;
    }

    /* n == 2 here (see the comment above): the one and only real,
     * permanent allocation this function ever makes, built directly
     * from the last pairwise merge's own output -- no extra copy. */
    void *mem = ll_arena_alloc(LL_ATTRS_OFF_ENTRIES + (uint64_t)(entries_cnt[0] + entries_cnt[1]) * LL_ATTR_SIZE);
    int64_t final_count = attrs_merge_entries_into(entries_ptr[0], entries_cnt[0], entries_ptr[1], entries_cnt[1],
                                                     (char *)mem + LL_ATTRS_OFF_ENTRIES);
    *(int64_t *)((char *)mem + LL_ATTRS_OFF_COUNT) = final_count;
    ll_stats_attrs++;
    ll_stats_attrs_entries += (uint64_t)final_count;

    for (int64_t s = 0; s < n_scratch; s++) free(scratch[s]);

    Value acc = (Value){ .raw = (uint64_t)(uintptr_t)mem | LL_TAG_ATTRS };
    *(Value *)((char *)base + LL_ATTRS_UNION_OFF_FLATCACHE) = acc;
    return acc;
}

int64_t ll_bshared_attrs_count(Value v) {
    v = ll_bshared_attrs_resolve(v);
    return *(const int64_t *)((const char *)(uintptr_t)(v.raw & ~(uint64_t)LL_TAG_MASK) + LL_ATTRS_OFF_COUNT);
}

uint32_t ll_bshared_attrs_sym(Value v, int64_t i) {
    v = ll_bshared_attrs_resolve(v);
    const char *entry =
        (const char *)(uintptr_t)(v.raw & ~(uint64_t)LL_TAG_MASK) + LL_ATTRS_OFF_ENTRIES + i * LL_ATTR_SIZE;
    return *(const uint32_t *)(entry + LL_ATTR_OFF_SYM);
}

Value ll_bshared_attrs_value(Value v, int64_t i) {
    v = ll_bshared_attrs_resolve(v);
    const char *entry =
        (const char *)(uintptr_t)(v.raw & ~(uint64_t)LL_TAG_MASK) + LL_ATTRS_OFF_ENTRIES + i * LL_ATTR_SIZE;
    return *(const Value *)(entry + LL_ATTR_OFF_VALUE);
}

/* Search on a value ALREADY KNOWN to be flat (count >= 0) -- shared by
 * ll_bshared_attrs_probe and ll_bshared_attrs_find below, raw-layout
 * version without any resolve call, since every leaf of a union is
 * already guaranteed flat by construction (ll_attrs_update always
 * resolves a would-be leaf before storing it), and a caller that has
 * already resolved its own `attrs` argument once knows the result is
 * flat too.
 *
 * Linear scan, not binary search, whenever the set has 8 or fewer
 * entries: a sequential scan over a small, tightly-packed entry array
 * beats binary search's O(log n) but branch-heavy, cache-unpredictable
 * jump pattern for small n, and most real nixpkgs attrsets (package
 * metadata, module options) are small. */
static int64_t attrs_find_flat_noresolve(Value flat, uint32_t sym) {
    const char *base = (const char *)(uintptr_t)(flat.raw & ~(uint64_t)LL_TAG_MASK);
    int64_t hi = *(const int64_t *)(base + LL_ATTRS_OFF_COUNT);
    const char *entries = base + LL_ATTRS_OFF_ENTRIES;
    if (hi <= 8) {
        const char *entry = entries;
        for (int64_t i = 0; i < hi; i++, entry += LL_ATTR_SIZE) {
            if (*(const uint32_t *)(entry + LL_ATTR_OFF_SYM) == sym) return i;
        }
        return -1;
    }
    int64_t lo = 0;
    while (lo < hi) {
        int64_t mid = lo + (hi - lo) / 2;
        const char *entry = entries + mid * LL_ATTR_SIZE;
        uint32_t s = *(const uint32_t *)(entry + LL_ATTR_OFF_SYM);
        if (s == sym) return mid;
        if (s < sym) lo = mid + 1; else hi = mid;
    }
    return -1;
}

int64_t ll_bshared_attrs_find(Value attrs, uint32_t sym) {
    attrs = ll_bshared_attrs_resolve(attrs);
    return attrs_find_flat_noresolve(attrs, sym);
}

/* Bounded multi-leaf probe for a SINGLE-KEY lookup on a lazy union
 * (heap.h's UnionHdr), without materializing/caching the full merge
 * first. Walks leaves from HIGHEST priority (last-stored, matching
 * `//`'s right-biased override semantics -- heap.h's own comment
 * confirms `leaves` is stored lowest-priority-first) to lowest,
 * binary-searching each flat leaf directly, returning on first hit.
 * Never touches flat_cache, never allocates, never mutates the union.
 * If the union was ALREADY materialized by some earlier full-iteration
 * caller, probes that cached flat form directly instead.
 *
 * Deliberately narrow in scope: only wired into op_select.S's single-key
 * `a.b` path, NOT into ll_bshared_attrs_find/_count/_sym/_value, whose
 * callers expect a stable POSITIONAL index into v's own flat form --
 * changing that contract to avoid materialization too would need
 * auditing every one of those call sites. */
int ll_bshared_attrs_probe(Value attrs, uint32_t sym, Value *out_value) {
    const char *base = (const char *)(uintptr_t)(attrs.raw & ~(uint64_t)LL_TAG_MASK);
    int64_t count = *(const int64_t *)(base + LL_ATTRS_OFF_COUNT);
    Value probe_target = attrs;
    if (count < 0) {
        Value cached = *(const Value *)(base + LL_ATTRS_UNION_OFF_FLATCACHE);
        if (cached.raw != 0) {
            probe_target = cached;
        } else {
            int64_t nleaves = -count;
            const Value *leaves = (const Value *)(base + LL_ATTRS_UNION_OFF_LEAVES);
            for (int64_t i = nleaves - 1; i >= 0; i--) {
                int64_t idx = attrs_find_flat_noresolve(leaves[i], sym);
                if (idx < 0) continue;
                const char *lbase = (const char *)(uintptr_t)(leaves[i].raw & ~(uint64_t)LL_TAG_MASK);
                const char *entry = lbase + LL_ATTRS_OFF_ENTRIES + idx * LL_ATTR_SIZE;
                *out_value = *(const Value *)(entry + LL_ATTR_OFF_VALUE);
                return 1;
            }
            return 0;
        }
    }
    int64_t idx = attrs_find_flat_noresolve(probe_target, sym);
    if (idx < 0) return 0;
    const char *pbase = (const char *)(uintptr_t)(probe_target.raw & ~(uint64_t)LL_TAG_MASK);
    const char *entry = pbase + LL_ATTRS_OFF_ENTRIES + idx * LL_ATTR_SIZE;
    *out_value = *(const Value *)(entry + LL_ATTR_OFF_VALUE);
    return 1;
}

typedef struct {
    uint32_t sym;
    int64_t idx;
} MakeAttrsEntry;

/* Threshold below which plain insertion sort beats a merge sort's loop/
 * merge overhead, plus any scratch allocation at all -- the same
 * tradeoff, and the same constant, as compiler.c's AST_ATTRSET case uses
 * for its own compile-time sort (not shared via a header since the two
 * sorts operate on unrelated data and this file has no other reason to
 * depend on compiler.c). */
#define LL_MAKE_ATTRS_SORT_INSERTION_MAX 32

/* Bottom-up (iterative) merge sort over `.sym` alone, specialized for
 * `MakeAttrsEntry`. Naturally stable (ties always resolved in favor of
 * the left run, i.e. the earlier original index) by construction, so it
 * reproduces an explicit `(sym, idx)` tie-break without ever comparing
 * `.idx` at all -- a duplicate sym's earlier occurrence always stays
 * first. `tmp` must be a second buffer of the same size as `a`; returns
 * whichever of `a`/`tmp` ends up holding the fully-sorted result
 * (alternates each pass, so for odd counts of passes it is `tmp`). */
static MakeAttrsEntry *merge_sort_make_attrs_entries(MakeAttrsEntry *a, MakeAttrsEntry *tmp, int64_t n) {
    MakeAttrsEntry *src = a, *dst = tmp;
    for (int64_t width = 1; width < n; width *= 2) {
        for (int64_t i = 0; i < n; i += 2 * width) {
            int64_t mid = i + width < n ? i + width : n;
            int64_t end = i + 2 * width < n ? i + 2 * width : n;
            int64_t l = i, r = mid, k = i;
            while (l < mid && r < end) {
                if (src[l].sym <= src[r].sym) dst[k++] = src[l++];
                else dst[k++] = src[r++];
            }
            while (l < mid) dst[k++] = src[l++];
            while (r < end) dst[k++] = src[r++];
        }
        MakeAttrsEntry *t = src; src = dst; dst = t;
    }
    return src;
}

Value ll_bshared_make_attrs(uint32_t *syms, Value *vals, int64_t count) {
    void *mem = ll_arena_alloc(8 + (uint64_t)count * LL_ATTR_SIZE);
    ll_stats_attrs++;
    ll_stats_attrs_entries += (uint64_t)count;
    *(int64_t *)((char *)mem + LL_ATTRS_OFF_COUNT) = count;

    if (count <= LL_MAKE_ATTRS_SORT_INSERTION_MAX) {
        int64_t order_stack[LL_MAKE_ATTRS_SORT_INSERTION_MAX];
        for (int64_t i = 0; i < count; i++) order_stack[i] = i;
        for (int64_t i = 1; i < count; i++) {
            int64_t j = i;
            while (j > 0 && syms[order_stack[j - 1]] > syms[order_stack[j]]) {
                int64_t tmp = order_stack[j - 1];
                order_stack[j - 1] = order_stack[j];
                order_stack[j] = tmp;
                j--;
            }
        }
        for (int64_t i = 0; i < count; i++) {
            char *entry = (char *)mem + LL_ATTRS_OFF_ENTRIES + i * LL_ATTR_SIZE;
            int64_t src = order_stack[i];
            /* Must be a 4-byte write -- entry's value bytes start right after sym (no pad),
             * so an 8-byte write here would clobber them. */
            *(uint32_t *)(entry + LL_ATTR_OFF_SYM) = syms[src];
            *(Value *)(entry + LL_ATTR_OFF_VALUE) = vals[src];
        }
    } else {
        MakeAttrsEntry *entries = ll_arena_alloc((uint64_t)count * sizeof(MakeAttrsEntry));
        MakeAttrsEntry *scratch = ll_arena_alloc((uint64_t)count * sizeof(MakeAttrsEntry));
        for (int64_t i = 0; i < count; i++) {
            entries[i].sym = syms[i];
            entries[i].idx = i;
        }
        MakeAttrsEntry *sorted = merge_sort_make_attrs_entries(entries, scratch, count);
        for (int64_t i = 0; i < count; i++) {
            char *entry = (char *)mem + LL_ATTRS_OFF_ENTRIES + i * LL_ATTR_SIZE;
            int64_t src = sorted[i].idx;
            *(uint32_t *)(entry + LL_ATTR_OFF_SYM) = sorted[i].sym;
            *(Value *)(entry + LL_ATTR_OFF_VALUE) = vals[src];
        }
    }
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | LL_TAG_ATTRS };
}

/* For every one of `__overrides`'s `ocount` entries, a naive scan over
 * all `n` of the enclosing `rec {}` block's static bindings
 * (`meta[0..n-1]`) looking for a same-sym match costs O(n*ocount)
 * overall. `rec {}`'s own static bindings are necessarily distinct syms
 * (a literal attrset/rec block can't declare the same name twice), so
 * sorting a (sym, original Env-slot index) side array once and
 * binary-searching it per override is enough. Same insertion-sort-below/
 * qsort-above threshold convention as make_attrs's
 * LL_MAKE_ATTRS_SORT_INSERTION_MAX. */
#define LL_REC_OVERRIDES_SORT_INSERTION_MAX 32

/* Building+sorting `order` unconditionally regresses real `rec{}` blocks
 * with `__overrides`, which overwhelmingly have few static bindings
 * (where the malloc+sort setup costs more than the linear scan it
 * replaces). Gated on the PRODUCT `n * ocount`, not `n` alone -- gating
 * on `n` alone would still pay sort overhead for a "few static bindings,
 * many override entries" shape where the OLD per-override linear scan
 * was already cheap because `n` itself was small. Below the threshold,
 * skip the index and fall back to the original O(n * ocount)
 * linear-scan-per-override behavior. */
#define LL_REC_OVERRIDES_PRODUCT_MAX 2048

typedef struct { uint32_t sym; int64_t idx; } RecOverridesEntry;

static int cmp_rec_overrides_entry(const void *a, const void *b) {
    const RecOverridesEntry *ea = a, *eb = b;
    if (ea->sym != eb->sym) return ea->sym < eb->sym ? -1 : 1;
    return 0;
}

static int64_t rec_overrides_find(const RecOverridesEntry *sorted, int64_t n, uint32_t sym) {
    int64_t lo = 0, hi = n - 1;
    while (lo <= hi) {
        int64_t mid = lo + (hi - lo) / 2;
        if (sorted[mid].sym == sym) return sorted[mid].idx;
        if (sorted[mid].sym < sym) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

Value ll_bshared_rec_overrides_merge(Env *env, int64_t n, const int64_t *meta) {
    int64_t overrides_slot = meta[n];
    Value overrides_val = ll_force_for_print(env->values[overrides_slot]);
    if (ll_tag_of(overrides_val) != LL_TAG_ATTRS) {
        fprintf(stderr, "ll: '__overrides' must be a set (got %s)\n",
                ll_bshared_type_name(overrides_val));
        exit(1);
    }

    /* Working (sym, value) pairs: the n static bindings first (by Env
     * slot order, matched ones replaced below), then any override-only
     * extras appended -- ll_bshared_make_attrs sorts the whole lot by
     * sym at the end, so this function never needs to track order itself. */
    int64_t ocount = ll_bshared_attrs_count(overrides_val);
    int64_t cap = n + ocount;
    uint32_t *syms = malloc((size_t)cap * sizeof(uint32_t));
    Value *vals = malloc((size_t)cap * sizeof(Value));
    for (int64_t i = 0; i < n; i++) {
        syms[i] = (uint32_t)meta[i];
        vals[i] = env->values[i];
    }

    /* Gated on the PRODUCT (see LL_REC_OVERRIDES_PRODUCT_MAX's own
     * comment above) -- below that, `order` is never built and the loop
     * below falls back to the original linear scan untouched. */
    int use_sorted = n * ocount > LL_REC_OVERRIDES_PRODUCT_MAX;
    RecOverridesEntry *order = NULL;
    if (use_sorted) {
        order = malloc((size_t)n * sizeof(RecOverridesEntry));
        for (int64_t i = 0; i < n; i++) order[i] = (RecOverridesEntry){ .sym = syms[i], .idx = i };
        if (n <= LL_REC_OVERRIDES_SORT_INSERTION_MAX) {
            for (int64_t i = 1; i < n; i++) {
                RecOverridesEntry e = order[i];
                int64_t j = i;
                while (j > 0 && order[j - 1].sym > e.sym) {
                    order[j] = order[j - 1];
                    j--;
                }
                order[j] = e;
            }
        } else {
            qsort(order, (size_t)n, sizeof(RecOverridesEntry), cmp_rec_overrides_entry);
        }
    }

    int64_t total = n;
    for (int64_t oi = 0; oi < ocount; oi++) {
        uint32_t osym = ll_bshared_attrs_sym(overrides_val, oi);
        Value oval = ll_bshared_attrs_value(overrides_val, oi);
        int64_t match;
        if (use_sorted) {
            match = rec_overrides_find(order, n, osym);
        } else {
            match = -1;
            for (int64_t i = 0; i < n; i++) {
                if (syms[i] == osym) { match = i; break; }
            }
        }
        if (match >= 0) {
            vals[match] = oval;
            env->values[match] = oval; /* propagate into sibling self-references */
        } else {
            syms[total] = osym;
            vals[total] = oval;
            total++;
        }
    }
    free(order);
    Value out = ll_bshared_make_attrs(syms, vals, total);
    free(syms);
    free(vals);
    return out;
}

void ll_bshared_string_bytes(Value v, const char **bytes_out, int64_t *len_out, const char *who) {
    if (ll_tag_of(v) != LL_TAG_PTR) {
        fprintf(stderr, "ll: %s: expected a string argument\n", who);
        exit(1);
    }
    *bytes_out = ll_ptr_bytes(v, len_out);
}

/* Real Nix's `eqValues` special-cases attrsets that denote a derivation
 * (`isDerivation(v)`: a `type` attribute whose forced value is exactly
 * the string "derivation") -- comparing them by their `outPath`
 * attribute's value (recursively) rather than the generic
 * attribute-by-attribute structural walk below. Needed because the
 * generic structural walk would otherwise (a) give the wrong answer
 * whenever a `//` merge changes the attribute count/shape without
 * changing outPath, and (b) risk forcing attributes real Nix never would
 * (e.g. a `throw`-valued attribute merged on afterward) just to decide
 * structural equality. */
static int value_is_derivation(Value v) {
    if (ll_tag_of(v) != LL_TAG_ATTRS) return 0;
    uint32_t sym_type = ll_intern("type", 4);
    int64_t idx = ll_bshared_attrs_find(v, sym_type);
    if (idx < 0) return 0;
    Value tv = ll_force_for_print(ll_bshared_attrs_value(v, idx));
    if (ll_tag_of(tv) != LL_TAG_PTR || ll_value_is_path(tv) || ll_value_is_float(tv)) return 0;
    int64_t len;
    const char *bytes = ll_ptr_bytes(tv, &len);
    return len == 10 && memcmp(bytes, "derivation", 10) == 0;
}

/* Real Nix's `eqValues` starts with `if (&v1 == &v2) return true` --
 * comparing the ADDRESS of the Value slot/cell itself, BEFORE forcing
 * either side, not the heap payload either side eventually forces to.
 * This evaluator's closest equivalents are (a) two Cells whose `target`
 * fields point at the exact same Env slot, and (b) two Values that are
 * the literal same raw THUNK. Must run on the RAW, pre-force operands --
 * forcing loses exactly the identity information being checked.
 *
 * Deliberately does NOT do a blanket `a.raw == b.raw` on any already-
 * concrete tag (INT/IMM/PTR/LIST/ATTRS/CLOSURE/PRIMOP): two closures that
 * happen to be bit-identical CONTENT (e.g. `let f = x: x; in f == f`,
 * both reads forced down to the same underlying closure object) are NOT
 * "the same slot" in real Nix's sense and must still compare `false` --
 * a blanket raw-equality check would wrongly short-circuit this to
 * `true`. List/Attrs pointer-equality is still handled, correctly, by
 * ll_bshared_value_eq's own LIST/ATTRS cases below (content equality
 * there IS genuine equality, unlike for a function). */
static int ll_bshared_slot_identical(Value a, Value b) {
    if (ll_value_is_cell(a) && ll_value_is_cell(b)) return ll_cell_target(a) == ll_cell_target(b);
    if (ll_tag_of(a) == LL_TAG_THUNK && ll_tag_of(b) == LL_TAG_THUNK) return a.raw == b.raw;
    return 0;
}

int ll_bshared_value_eq(Value a, Value b) {
    if (ll_bshared_slot_identical(a, b)) return 1;
    a = ll_force_for_print(a);
    b = ll_force_for_print(b);
    int ta = ll_tag_of(a), tb = ll_tag_of(b);
    /* Float check runs FIRST: before the generic PTR-vs-PTR string/path
     * byte-compare below (a Float's bytes are a marker+double, not
     * length-prefixed text, so that branch would misread them) and
     * before the plain `ta != tb` type-mismatch rejection (int-vs-float
     * must still compare equal when numerically equal, e.g. `1 == 1.0`
     * is `true`). Not numeric on one side and not float-tagged on the
     * other (e.g. a Float vs a string) simply isn't equal, same as any
     * other cross-type `==`. */
    if (ll_value_is_float(a) || ll_value_is_float(b)) {
        double da, db;
        if (ll_value_is_float(a)) da = ll_float_value(a);
        else if (ta == LL_TAG_INT) da = (double)ll_int_value(a);
        else return 0;
        if (ll_value_is_float(b)) db = ll_float_value(b);
        else if (tb == LL_TAG_INT) db = (double)ll_int_value(b);
        else return 0;
        return da == db;
    }
    if (ta == LL_TAG_PTR && tb == LL_TAG_PTR) {
        int64_t la, lb;
        const char *ba = ll_ptr_bytes(a, &la);
        const char *bb = ll_ptr_bytes(b, &lb);
        return la == lb && (la == 0 || memcmp(ba, bb, (size_t)la) == 0);
    }
    if (ta != tb) return 0;
    switch (ta) {
        case LL_TAG_INT: return ll_int_value(a) == ll_int_value(b);
        case LL_TAG_IMM: return ll_imm_subtag(a) == ll_imm_subtag(b);
        case LL_TAG_LIST: {
            if (a.raw == b.raw) return 1;
            int64_t na = ll_bshared_list_count(a), nb = ll_bshared_list_count(b);
            if (na != nb) return 0;
            const Value *ea = ll_bshared_list_elems(a), *eb = ll_bshared_list_elems(b);
            for (int64_t i = 0; i < na; i++) {
                if (!ll_bshared_value_eq(ea[i], eb[i])) return 0;
            }
            return 1;
        }
        case LL_TAG_ATTRS: {
            if (a.raw == b.raw) return 1;
            if (value_is_derivation(a) && value_is_derivation(b)) {
                uint32_t sym_outpath = ll_intern("outPath", 7);
                int64_t ia = ll_bshared_attrs_find(a, sym_outpath);
                int64_t ib = ll_bshared_attrs_find(b, sym_outpath);
                if (ia >= 0 && ib >= 0) {
                    return ll_bshared_value_eq(ll_bshared_attrs_value(a, ia), ll_bshared_attrs_value(b, ib));
                }
            }
            int64_t na = ll_bshared_attrs_count(a), nb = ll_bshared_attrs_count(b);
            if (na != nb) return 0;
            for (int64_t i = 0; i < na; i++) {
                if (ll_bshared_attrs_sym(a, i) != ll_bshared_attrs_sym(b, i)) return 0;
                if (!ll_bshared_value_eq(ll_bshared_attrs_value(a, i), ll_bshared_attrs_value(b, i)))
                    return 0;
            }
            return 1;
        }
        /* Closures/primops (the only tags that can still reach here --
         * INT/IMM/PTR/LIST/ATTRS are all handled above, and THUNK is
         * impossible: force_for_print already fully resolved both sides)
         * are NEVER equal at this point, matching real Nix's `case
         * nFunction: return false`, which runs unconditionally once the
         * slot-identity shortcut (ll_bshared_slot_identical, checked
         * above on the raw pre-force operands) hasn't already fired.
         * Do NOT fall back to `a.raw == b.raw` (forced-object pointer
         * identity) here: that gives the WRONG answer (true) whenever
         * two INDEPENDENTLY-computed closures happen to evaluate to the
         * literal same heap object -- e.g. `[(id id)] == [(id id)]`,
         * which real Nix says is `false`. `ll_bshared_slot_identical`
         * above is what keeps the cases that DO need `true` (the same
         * shared Thunk read twice; a bare-variable aliased pair via a
         * Cell) working. */
        default: return 0;
    }
}

/* See builtins_shared.h's comment. Also checks the Cell slot-identity
 * shortcut (ll_bshared_slot_identical) on the RAW, PRE-FORCE `a`/`b` --
 * before EITHER of this function's own two `force_for_print` calls,
 * which would otherwise discard exactly the aliasing information being
 * checked. */
int ll_bshared_value_eq_op(Value a, Value b) {
    if (ll_bshared_slot_identical(a, b)) return 1;
    Value fa = ll_force_for_print(a);
    Value fb = ll_force_for_print(b);
    int ta = ll_tag_of(fa), tb = ll_tag_of(fb);
    if (ta == tb && (ta == LL_TAG_CLOSURE || ta == LL_TAG_PRIMOP)) return 0;
    return ll_bshared_value_eq(fa, fb);
}

Value ll_bshared_select_dynamic(Value target, Value name_val) {
    if (ll_tag_of(target) != LL_TAG_ATTRS) {
        fprintf(stderr, "ll: cannot select an attribute (dynamic) on a non-attrset value (got %s)\n",
                ll_bshared_type_name(target));
        exit(1);
    }
    if (ll_tag_of(name_val) != LL_TAG_PTR || ll_value_is_path(name_val) || ll_value_is_float(name_val)) {
        fprintf(stderr, "ll: a dynamic '.${...}' attribute name must be a string (got %s)\n",
                ll_bshared_type_name(name_val));
        exit(1);
    }
    int64_t len;
    const char *bytes = ll_ptr_bytes(name_val, &len);
    uint32_t sym = ll_intern(bytes, (size_t)len);
    int64_t idx = ll_bshared_attrs_find(target, sym);
    if (idx < 0) {
        fprintf(stderr, "ll: attribute '%.*s' missing, aborting\n", (int)len, bytes);
        exit(1);
    }
    return ll_force_for_print(ll_bshared_attrs_value(target, idx));
}

/* `expr.${nameExpr} or default` -- OP_SELECT_DYNAMIC_OR's C half. Unlike
 * ll_bshared_select_dynamic above (fatal on a non-attrset target or a
 * missing key), a non-attrset target here falls through gracefully to
 * the default too -- only the name itself being a non-string is still a
 * hard fatal (that's a genuine misuse, not a "not found"). Returns 1 and
 * writes the forced value through `*out` on a hit, 0 (leaving `*out`
 * untouched) on a miss -- the caller (op_select_dynamic_or.S) decides
 * whether to push the value or jump to the default's code. */
int ll_bshared_select_dynamic_or(Value target, Value name_val, Value *out) {
    if (ll_tag_of(target) != LL_TAG_ATTRS) return 0;
    if (ll_tag_of(name_val) != LL_TAG_PTR || ll_value_is_path(name_val) || ll_value_is_float(name_val)) {
        fprintf(stderr, "ll: a dynamic '.${...}' attribute name must be a string (got %s)\n",
                ll_bshared_type_name(name_val));
        exit(1);
    }
    int64_t len;
    const char *bytes = ll_ptr_bytes(name_val, &len);
    uint32_t sym = ll_intern(bytes, (size_t)len);
    int64_t idx = ll_bshared_attrs_find(target, sym);
    if (idx < 0) return 0;
    *out = ll_force_for_print(ll_bshared_attrs_value(target, idx));
    return 1;
}

int ll_bshared_has_attr_dynamic(Value target, Value name_val) {
    if (ll_tag_of(target) != LL_TAG_ATTRS) return 0;
    if (ll_tag_of(name_val) != LL_TAG_PTR || ll_value_is_path(name_val) || ll_value_is_float(name_val)) {
        fprintf(stderr, "ll: a dynamic '${...}' attribute name must be a string (got %s)\n",
                ll_bshared_type_name(name_val));
        exit(1);
    }
    int64_t len;
    const char *bytes = ll_ptr_bytes(name_val, &len);
    uint32_t sym = ll_intern(bytes, (size_t)len);
    return ll_bshared_attrs_find(target, sym) >= 0;
}

int ll_bshared_value_lt(Value a, Value b, const char *who) {
    a = ll_force_for_print(a);
    b = ll_force_for_print(b);
    int ta = ll_tag_of(a), tb = ll_tag_of(b);
    if (ta == LL_TAG_INT && tb == LL_TAG_INT) {
        return ll_int_value(a) < ll_int_value(b);
    }
    if (ta == LL_TAG_PTR && tb == LL_TAG_PTR && !ll_value_is_path(a) && !ll_value_is_path(b) &&
        !ll_value_is_float(a) && !ll_value_is_float(b)) {
        int64_t la, lb;
        const char *ba = ll_ptr_bytes(a, &la);
        const char *bb = ll_ptr_bytes(b, &lb);
        int64_t n = la < lb ? la : lb;
        int c = n > 0 ? memcmp(ba, bb, (size_t)n) : 0;
        if (c != 0) return c < 0;
        return la < lb;
    }
    /* Lists order lexicographically, recursively comparing elements
     * pairwise (shorter-is-less on a common prefix). A type mismatch
     * between two elements at the first differing index is the same
     * fatal "cannot compare" error as a top-level mismatch. */
    if (ta == LL_TAG_LIST && tb == LL_TAG_LIST) {
        int64_t na = ll_bshared_list_count(a), nb = ll_bshared_list_count(b);
        const Value *ea = ll_bshared_list_elems(a), *eb = ll_bshared_list_elems(b);
        int64_t n = na < nb ? na : nb;
        for (int64_t i = 0; i < n; i++) {
            /* ll_bshared_value_eq on the RAW (pre-force) elements -- do
             * NOT force_for_print them first: forcing here would strip
             * exactly the slot/Cell identity ll_bshared_value_eq's own
             * shortcut needs to let two bare-var-aliased elements (e.g.
             * both lists' shared `id`) short-circuit as "equal, compare
             * the next element" instead of wrongly falling through to
             * the fatal "cannot compare these two values" case below. */
            if (ll_bshared_value_eq(ea[i], eb[i])) continue;
            Value ea_i = ll_force_for_print(ea[i]);
            Value eb_i = ll_force_for_print(eb[i]);
            return ll_bshared_value_lt(ea_i, eb_i, who);
        }
        return na < nb;
    }
    fprintf(stderr, "ll: %s: cannot compare these two values (only int-vs-int, string-vs-string, and list-vs-list are supported)\n",
            who);
    exit(1);
}
