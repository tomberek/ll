/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 src/asm/ops/op_check_formals.S, op_assert.S, op_insert_dynamic_attr.S, op_rec_overrides_merge.S, op_resolve_search_path.S -- see soft-wishing-engelbart.md plan
 *
 * Five independent, lower-traffic ops, all deferred (same as asnix's own
 * asm) to a single vendored C helper each -- this file is just SysV/
 * dispatch glue, no hand-rolled logic duplicating what the vendored
 * helpers already do. */
#include "ops.h"

#include "builtins_shared.h"
#include "attrs_update.h"
#include "tryeval.h"

/* asnix_resolve_search_path_value (compiler.c) is non-static (asm calls
 * it directly via `call` with no header -- see op_resolve_search_path.S)
 * but, unlike every other vendored helper this file uses, it has no
 * prototype in any src/c header -- compiler.h only declares
 * asnix_compile/asnix_compile_scoped. Declared here instead of adding
 * a new header dependency for one function. */
extern Value asnix_resolve_search_path_value(Value name_val);

/* OP_CHECK_FORMALS: a = required_count, b = pool_idx -- see
 * op_check_formals.S's long header comment for the exact pool layout:
 *   pool[b+0]                          = total declared-field count, or
 *       -1 if the pattern has `...` (skips phase 2 entirely)
 *   pool[b+1 .. b+required_count]      = required fields' symbol ids,
 *       sorted ascending (required_count = ip->a)
 *   pool[b+1+required_count .. b+total_count] = every declared field's
 *       symbol id, sorted ascending (only present/read when `...` is
 *       absent)
 * Pops the Attrs (fatal if not one -- same inline message asnix's own
 * .Lcf_type_error uses); pushes nothing back (net pop 1, push 0).
 * Unlike the asm's hand-rolled binary searches, phase 1 just reuses
 * asnix_bshared_attrs_find (builtins_shared.c), which already resolves
 * a lazy `//` union header internally (M88) and binary-searches --
 * exactly what the asm's own two-step "resolve then search" dance did
 * by hand. Phase 2 (the allowed-field check) binary-searches the pool's
 * own sorted region directly, since that's a plain int64 array, not an
 * Attrs object. */
Value ll_op_check_formals(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    Value attrs = vs[-1];
    vs--;
    if ((attrs.raw & ASNIX_TAG_MASK) != ASNIX_TAG_ATTRS) {
        static const char msg[] = "asnix: lambda pattern argument is not a set, aborting\n";
        asnix_fatal(msg, sizeof(msg) - 1);
    }

    int64_t required_count = ip->a;
    int64_t pool_idx = ip->b;

    for (int64_t i = 0; i < required_count; i++) {
        uint32_t sym = (uint32_t)pool[pool_idx + 1 + i];
        if (asnix_bshared_attrs_find(attrs, sym) < 0) {
            asnix_bshared_cf_missing_fatal(sym); /* _Noreturn */
        }
    }

    int64_t total_count = pool[pool_idx];
    if (total_count != -1) {
        int64_t allowed_base = pool_idx + 1 + required_count;
        int64_t n = asnix_bshared_attrs_count(attrs);
        for (int64_t j = 0; j < n; j++) {
            uint32_t sym = asnix_bshared_attrs_sym(attrs, j);
            int64_t lo = 0, hi = total_count;
            while (lo < hi) {
                int64_t mid = lo + (hi - lo) / 2;
                uint32_t allowed = (uint32_t)pool[allowed_base + mid];
                if (allowed == sym) {
                    goto found;
                } else if (allowed < sym) {
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }
            asnix_bshared_cf_extra_fatal(sym); /* _Noreturn */
        found:;
        }
    }

    LL_NEXT(ip, env, vs, pool, rs);
}

/* OP_ASSERT_FAIL: b = pool index of a {msg ptr, msg len} pair
 * (compiler.c-built, names the failed condition). Unconditionally
 * raises -- routed through asnix_tryeval_raise_or_fatal (tryeval.c), not
 * asnix_fatal directly, so an active `builtins.tryEval` can catch a
 * failed `assert` exactly like `throw` (M30); falls back to exactly
 * asnix_fatal's own write+exit(1) behavior when no tryEval is active.
 * No VALSTACK effect at all other than the abort -- never returns, so
 * there is no LL_NEXT/LL_GOTO here (same precedent as op_select.S's
 * _Noreturn-terminated branches, just for the whole handler this time). */
Value ll_op_assert_fail(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    (void)env;
    (void)vs;
    (void)rs;
    int64_t pool_idx = ip->b;
    const char *msg = (const char *)(intptr_t)pool[pool_idx];
    size_t len = (size_t)pool[pool_idx + 1];
    asnix_tryeval_raise_or_fatal(msg, len); /* _Noreturn */
}

/* OP_INSERT_DYNAMIC_ATTR: no operands. Pops value, then name, then attrs
 * (reverse push order -- compiler.c's AST_ATTRSET case pushes the
 * already-built static attrs first, then the dynamic name, then the
 * value thunk -- confirmed against op_insert_dynamic_attr.S's own
 * `16(%r12)`=value/`8(%r12)`=name/`(%r12)`=attrs loads). Interning,
 * the string-type check, the duplicate-key fatal, and the sorted-
 * insert are all handled by asnix_attrs_insert_dynamic itself
 * (attrs_update.c) -- pushes its result (net pop 3, push 1). */
Value ll_op_insert_dynamic_attr(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    Value value = vs[-1];
    Value name = vs[-2];
    Value attrs = vs[-3];
    vs -= 3;

    Value result = asnix_attrs_insert_dynamic(attrs, name, value);

    vs[0] = result;
    vs++;
    LL_NEXT(ip, env, vs, pool, rs);
}

/* OP_REC_OVERRIDES_MERGE: a = n (the rec's own static binding count),
 * b = pool index of n+1 int64 entries -- meta[0..n-1] are the n static
 * bindings' symbol ids in original (= Env slot) order, meta[n] is which
 * of those n slots holds `__overrides` itself (see ir.h's own long
 * comment for the full picture). Unlike every other op in this file,
 * this one pops NOTHING off VALSTACK -- its inputs are the CURRENT
 * Env's own slots (already filled in by the OP_FRAME_SET loop
 * compiler.c emitted just before this op), not values pushed by the
 * compiler. asnix_bshared_rec_overrides_merge does the real work
 * (forcing the `__overrides` slot, merging, and overwriting matched
 * Env slots in place) and returns the one finished Attrs Value this
 * pushes. */
Value ll_op_rec_overrides_merge(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    int64_t n = ip->a;
    const int64_t *meta = &pool[ip->b];

    Value result = asnix_bshared_rec_overrides_merge(env, n, meta);

    ll_check_valstack(vs);
    vs[0] = result;
    vs++;
    LL_NEXT(ip, env, vs, pool, rs);
}

/* OP_RESOLVE_SEARCH_PATH: no operands. Pops a String (the search name,
 * pushed as an ordinary compile-time constant by compiler.c's AST_PATH
 * case -- no runtime type check needed here, matching the asm, which
 * passes the popped Value straight through with no tag check of its
 * own). Pushes the resolved, canonicalized Path -- or
 * asnix_resolve_search_path_value itself never returns at all (a lookup
 * miss routes through asnix_tryeval_raise_or_fatal, same catchable
 * mechanism `throw`/a failed `assert` use), in which case the push below
 * never runs. */
Value ll_op_resolve_search_path(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    Value name = vs[-1];
    vs--;

    Value result = asnix_resolve_search_path_value(name);

    vs[0] = result;
    vs++;
    LL_NEXT(ip, env, vs, pool, rs);
}
