/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_BUILTINS_SHARED_H
#define LL_BUILTINS_SHARED_H

#include <stddef.h>
#include <stdint.h>

#include "ll/env.h" /* Env, for ll_bshared_rec_overrides_merge */
#include "ll/value.h"
#include "builtins_list.h" /* shared LlPrimFn/LlBuiltinDef typedefs */

/*
 * Shared helpers for this pass's new builtins modules (builtins_closure.c,
 * builtins_list2.c, builtins_attrs2.c, builtins_compare.c -- see
 * docs/architecture.md's "builtins expansion, round 2" section).
 *
 * M17's three sibling modules (builtins_list.c/builtins_attrs.c/
 * builtins_misc.c) each deliberately *duplicated* small helpers
 * (make_primop_value, list/attrs raw accessors, ...) instead of sharing
 * them, specifically because three different agents were writing those
 * three files in parallel and sharing a helper file would have meant
 * textually colliding on it (see builtins_misc.c's header comment,
 * "deliberately re-declared here rather than shared"). That reason does
 * not apply this time: one agent is writing all four new modules in this
 * pass, so there is no parallel-collision risk to avoid, and four-way
 * duplication of a ~30-line curry helper (make_curried_native,
 * builtins_misc.c) plus a dozen tiny List/Attrs accessors would just be
 * real, unmotivated duplication with no upside. One precedent for sharing
 * already exists in the established code (builtins_attrs.h includes
 * builtins_list.h for the LlPrimFn/LlBuiltinDef shape rather than
 * redefining it) -- this header is that same judgment call applied one
 * step further, not a new pattern.
 */

Value ll_bshared_make_primop(void *fn, int64_t arity);

/* Same as ll_bshared_make_primop, except the resulting PrimOp's
 * arguments are NOT pre-forced by op_call.S before the native function
 * is called (LL_PRIMOP_OFF_NOFORCE, heap.h) -- used only by
 * `tryEval` (M30), which needs its raw, unforced argument so it can
 * force it itself inside its own setjmp/longjmp catch scope. */
Value ll_bshared_make_primop_noforce(void *fn, int64_t arity);

/* Builds a curried Closure chain of length `arity` wrapping the real,
 * ordinary arity-`arity` native PrimOp `native_fn` -- see
 * builtins_misc.c's make_curried_native for the full rationale (ordinary
 * Nix-level application always desugars to nested arity-1 calls --
 * parser.c's parse_app -- so a true arity-2+ builtin can't be registered
 * as a single multi-arg PrimOp; this builds the same "x: y: ... body"-
 * shaped Closure chain a real curried user lambda would compile to). This
 * is a fresh port of that file's logic (which is `static`), used by every
 * arity>=2 builtin added in this pass. */
Value ll_bshared_make_curried_native(void *native_fn, int64_t arity);

/* M92: same curry-chain shape as ll_bshared_make_curried_native just
 * above, EXCEPT the generated chain reassembles its captured arguments
 * with OP_VAR_REF_RAW (ib_var_ref_raw) instead of plain OP_VAR_REF, and
 * the leaf native call is built via ll_bshared_make_primop_noforce
 * instead of ll_bshared_make_primop -- so NEITHER the curry chain's
 * own reassembly NOR op_call.S's post-dispatch primop-arg-forcing loop
 * (M25) ever forces any argument before `native_fn` runs; `native_fn`
 * is entirely responsible for forcing whatever it needs itself. Needed
 * by `elem` (builtins_list2.c): its first argument must reach
 * ll_bshared_value_eq completely raw/unforced, so that function's own
 * slot/Cell-identity shortcut (see its M92 comment) can still fire --
 * forcing it first (what ll_bshared_make_curried_native's plain
 * OP_VAR_REF reassembly does, harmlessly for every OTHER curried
 * builtin, since op_call.S's own M25 force-loop would force it right
 * back anyway) throws away exactly that identity information before
 * `elem`'s own body ever sees it (confirmed via numtide/nix-
 * conformance's eval-okay-eq-slot-elem.nix). Safe to use for OTHER
 * arguments too (as `elem`'s own `list` argument does) as long as the
 * native function forces every argument it actually needs the content
 * of -- same discipline tryEval's own ll_bshared_make_primop_noforce
 * already requires of its one caller. */
Value ll_bshared_make_curried_native_noforce(void *native_fn, int64_t arity);

/* M80: a single, lazily-allocated, byte-correct empty-attrset Value
 * (count=0), shared by every curry-stage Closure this file's/
 * builtins_misc.c's/builtins_m54.c's/builtins_attrs.c's native-builtin-
 * currying helpers (ll_bshared_make_curried_native, builtins_misc.c's
 * own make_curried_native, builtins_m54.c's make_curried_native_noforce,
 * builtins_attrs.c's make_curried2) build for their Closures' OFF_FORMALS
 * field -- see main.c's LL_TAG_CLOSURE print case for why this needs
 * to be ONE canonical address rather than each call site's own fresh
 * `ll_bshared_make_attrs(NULL, NULL, 0)` (which is what every one of
 * those call sites used before M80, and is still exactly what this
 * function's first call computes/returns -- so `builtins.functionArgs`
 * on any of these closures is completely unaffected, still a genuinely
 * valid, correct `{ }`). The only purpose this adds is a RELIABLE,
 * pointer-identity-based way for the printer to recognize "this Closure
 * is a native-builtin curry stage, not user Nix code": compiler.c's own
 * AST_LAMBDA case calls the plain `ll_bshared_make_attrs(NULL, NULL,
 * 0)` fresh for every single compiled non-pattern lambda (never this
 * function), so a real user lambda's formals pointer can never coincide
 * with this one, no matter how many are compiled. */
Value ll_bshared_native_formals_marker(void);

/* `fn a b` via two successive single-argument ll_apply calls -- same
 * shape as builtins_list.c's local (file-static) apply2. */
Value ll_bshared_apply2(Value fn, Value a, Value b);

void ll_bshared_fatal(const char *who, const char *msg);
/* OP_SELECT's (op_select.S) static-attribute-missing path -- see the
 * full comment in builtins_shared.c. */
_Noreturn void ll_bshared_select_missing_fatal(uint32_t sym);

/* M50: shared "what kind of value is this" helper for fatal-path error
 * messages -- see the long comment above its definition in
 * builtins_shared.c. Returns a static, non-owned "a/an <kind>" string
 * (int/bool/null/string/path/list/attrset/function/...), the same
 * tag -> name mapping builtins.c's `prim_typeOf` already established.
 * Non-static (unlike most one-off helpers in this file) because
 * attrs_update.c's ll_attrs_insert_dynamic needs it too. */
const char *ll_bshared_type_name(Value v);

/* OP_SELECT's (op_select.S) "selection target is not an attrset" fatal
 * path -- names the actual type via ll_bshared_type_name above. `v`
 * is the still-tagged Value popped off VALSTACK. */
_Noreturn void ll_bshared_select_type_fatal(Value v);

/* OP_CALL's (op_call.S) "call target is not a function" fatal path --
 * same idea. `v` is the still-tagged callee Value. */
_Noreturn void ll_bshared_call_type_fatal(Value v);

/* `__functor` protocol (M42) -- see the full comment in
 * builtins_shared.c. Called from both OP_CALL (op_call.S) and
 * ll_apply (apply.S) only after their own Closure/PrimOp dispatch
 * has already failed to match an attrset with a `__functor` attribute.
 * Fatal (reusing ll_bshared_call_type_fatal) if `__functor` is
 * absent. */
Value ll_bshared_call_functor(Value attrs_val, Value arg);

/* OP_CHECK_FORMALS's (op_check_formals.S) two lambda-pattern-mismatch
 * fatal paths -- names the specific required/unexpected field, mirroring
 * ll_bshared_select_missing_fatal's M36 precedent. `sym` is an
 * already-interned symbol id. */
_Noreturn void ll_bshared_cf_missing_fatal(uint32_t sym);
_Noreturn void ll_bshared_cf_extra_fatal(uint32_t sym);

void ll_bshared_require_tag(Value v, int tag, const char *who, const char *what);
#define LL_REQUIRE_ATTRS(v, who) ll_bshared_require_tag((v), LL_TAG_ATTRS, (who), "an attrset")
#define LL_REQUIRE_STRING(v, who) ll_bshared_require_tag((v), LL_TAG_PTR, (who), "a string")
#define LL_REQUIRE_LIST(v, who) ll_bshared_require_tag((v), LL_TAG_LIST, (who), "a list")
#define LL_REQUIRE_INT(v, who) ll_bshared_require_tag((v), LL_TAG_INT, (who), "an integer")

int64_t ll_bshared_list_count(Value v);
const Value *ll_bshared_list_elems(Value v);
Value ll_bshared_make_list(const Value *elems, int64_t count);

int64_t ll_bshared_attrs_count(Value v);
uint32_t ll_bshared_attrs_sym(Value v, int64_t i);
Value ll_bshared_attrs_value(Value v, int64_t i);
int64_t ll_bshared_attrs_find(Value attrs, uint32_t sym);

/* fork (unionhdr-probe): bounded multi-leaf probe, no materialization -- see builtins_shared.c */
int ll_bshared_attrs_probe(Value attrs, uint32_t sym, Value *out_value);

/* M88 (lazy `//` union-layering, heap.h's UnionHdr comment): `v` MUST
 * already be LL_TAG_ATTRS (callers check that first, same as every
 * other attrs_* accessor here). Returns `v` unchanged if it's already an
 * ordinary flat Attrs (the overwhelmingly common case: one sign-bit check,
 * no branch mispredict cost beyond that). If `v` is instead a UnionHdr,
 * materializes its fully flattened, deduped, sym-sorted equivalent on
 * first call (merging its (at most LL_ATTRS_UNION_MAX_LEAVES) leaves
 * left to right via ll_attrs_eager_merge, attrs_update.c) and caches
 * the result in the UnionHdr's own `flat_cache` slot -- every later call
 * on the SAME UnionHdr Value just returns the cached Value, no re-merge.
 * EVERY raw-layout reader of an ATTRS object (every op_*.S binary search,
 * this file's own count/sym/value/find below, derivation.c, main.c's
 * printer, builtins_attrs.c, builtins_m54.c's unsafeGetAttrPos) calls
 * this first and uses only ITS result -- see docs/architecture.md's M88
 * section for the full enumeration this was checked against. */
Value ll_bshared_attrs_resolve(Value v);

/* `syms`/`vals` need NOT be pre-sorted (sorted in place, ascending by sym
 * id, before building) -- unlike builtins_attrs.c's make_attrs_from_sorted
 * (which only ever inherits sortedness from an existing Attrs object),
 * several of this pass's builtins (partition's {right;wrong;}, listToAttrs)
 * build a brand new small attrset from freshly interned symbols whose
 * relative sym-id order depends on global intern history, not any order
 * this code controls -- sorting here, once, is simpler than making every
 * caller get that right itself. Plain insertion sort: these are always
 * small (see sorted_name_order's identical rationale in builtins_attrs.c). */
Value ll_bshared_make_attrs(uint32_t *syms, Value *vals, int64_t count);

/* `rec { ... __overrides = {...}; ... }` (M57c, OP_REC_OVERRIDES_MERGE --
 * see that op's long comment, ir.h, for the full picture). `env` is the
 * rec's own just-OP_FRAME_ALLOC'd Env (`rdi` at op-handler entry); `n` and
 * `meta` are the op's `a`/pool-at-`b` operands verbatim: `meta[0..n-1]`
 * are the n static bindings' symbol ids in original (= Env slot) order,
 * `meta[n]` is which of those n slots holds `__overrides` itself. Forces
 * `env->values[meta[n]]`, requires it's an Attrs (fatal otherwise), merges
 * it into the n static (sym, env->values[i]) pairs -- matched syms
 * overwrite BOTH the output pair's value AND env->values[i] itself (so
 * sibling bindings referencing that slot see the override), unmatched
 * override keys are appended as extra output-only pairs -- and returns
 * the one finished, freshly-sorted Attrs Value OP_REC_OVERRIDES_MERGE
 * pushes. */
Value ll_bshared_rec_overrides_merge(Env *env, int64_t n, const int64_t *meta);

/* Fatal if `v` isn't LL_TAG_PTR (string OR path -- same dual-kind
 * dispatch as value_util.c's ll_ptr_bytes; callers that specifically
 * need a *string*, not a path, should LL_REQUIRE_STRING first, which
 * this does NOT itself distinguish). */
void ll_bshared_string_bytes(Value v, const char **bytes_out, int64_t *len_out, const char *who);

/* Real structural equality, matching real Nix's `==`/`!=`: ints (numeric),
 * IMM (raw subtag -- true/false/null), strings/paths (byte content), lists
 * (same length, elementwise recursive equality), attrsets (same set of
 * attr names, recursive equality of each matching value) -- forcing any
 * thunk found along the way (list/attrset elements are lazy and not
 * pre-forced the way a bare variable reference is; see op_var_ref.S/
 * op_select.S) via the already-existing `ll_force_for_print` C-callable
 * force entry point (`ll/ir.h`), safe to call re-entrantly here for the
 * same reason it's already safe to call from a builtin or from inside
 * another nested force. Closures/primops (and thunks that are still
 * in-progress, though that can't happen reached from here) fall back to
 * pointer identity -- real Nix never structurally compares functions
 * either (confirmed: two syntactically-identical-but-distinct lambdas are
 * `!=`, see tests/conformance/equal-function-*.nix). This is the single
 * implementation both the `==`/`!=` operators (op_cmp.S, which calls this
 * directly) and dedup-style builtins (genericClosure, elem) use -- no
 * separate, narrower "scope cut" version exists anymore (see Known Bugs
 * entry below: the previous pointer-identity fallback for lists/attrsets
 * was a real, high-impact bug, not an acceptable scope cut, since `==` on
 * aggregates is pervasive in real nixpkgs code). */
int ll_bshared_value_eq(Value a, Value b);

/* M80: the `==`/`!=` OPERATORS' OWN two direct operands need one more rule
 * on top of ll_bshared_value_eq above: real Nix's `ExprOpEq::eval`
 * (eval.cc) always evaluates each of its two operands into a *fresh* local
 * `Value` (plain `eval()`, never the `maybeThunk()` which elides a new
 * allocation for a bare variable reference) before calling `eqValues`, so
 * `eqValues`'s own "hack to support some old broken code that relies on
 * pointer equality" (`if (&v1 == &v2) return true;`) can NEVER fire for
 * `==`'s own two top-level operands, no matter what expression either side
 * is -- confirmed against real Nix: `let f = x: x; in f == f` is `false`,
 * even though both sides are literally the same binding (see
 * tests/conformance or eval-okay-equal-function-direct-identical.nix in
 * numtide/nix-conformance). That pointer-identity hack DOES still fire one
 * level down inside a list/attrset (`[f] == [f]` is `true`: there, `==`'s
 * own two operands are two distinct list values, but the lists' sole
 * element came from the same variable slot, so *those* Values are
 * pointer-identical) -- asnix has no separate "Value cell" indirection (a
 * Value already IS its payload, see value.h) so it cannot replicate that
 * nested case in general; ll_bshared_value_eq's existing pointer-
 * identity-on-payload default for closures/primops is kept as-is for every
 * caller other than op_eq/op_neq themselves (recursive list/attrset
 * elements, and builtins.elem/genericClosure which call
 * ll_bshared_value_eq directly) since narrowing it further there
 * regresses other currently-passing cases (e.g. eval-okay-eq-slot-attrs)
 * without actually making eval-okay-eq-slot-elem/-identity pass either --
 * see docs/architecture.md's M80 notes for why those two stay out of scope.
 * This wrapper only strengthens the TOP-level, direct-operand case: if both
 * forced operands are the same function-ish tag (closure or primop), the
 * answer is unconditionally "not equal", matching ExprOpEq::eval exactly;
 * everything else defers entirely to ll_bshared_value_eq. */
int ll_bshared_value_eq_op(Value a, Value b);

/* Ordering used by lessThan/sort: INT<INT (numeric), string<string
 * (lexicographic byte compare, shorter-is-less on a shared prefix), and
 * (M57a) list<list (lexicographic element-by-element, recursing this same
 * function on the first differing pair -- shorter-is-less on an equal
 * common prefix) -- still narrower than real Nix's `lessThan` (which also
 * orders paths -- see ~/nix/src/libexpr/primops.cc's CompareValues),
 * matching the existing `<`/`>` operator's own INT-only scope cut
 * (op_cmp.S) extended just far enough to make `sort`/`lessThan`/
 * `genericClosure` useful on the string/list keys this subset's actual
 * callers have (nixpkgs-style name/version sorting, list-keyed closures).
 * Fatal on any other type, or a type mismatch, matching real Nix's error
 * class (not necessarily its exact wording). */
int ll_bshared_value_lt(Value a, Value b, const char *who);

/* Runtime implementation of `expr.${nameExpr}` (dynamic attribute select,
 * OP_SELECT_DYNAMIC -- see ir.h and op_select_dynamic.S). Fatal on a
 * non-attrset target, a non-string name, or a missing key (same error
 * class as plain OP_SELECT -- op_select.S), unlike OP_SELECT_OR's
 * graceful fallthrough (`.${expr} or default` isn't supported, see
 * parser.c). Forces the result if it's an unforced thunk, matching plain
 * OP_SELECT's laziness. */
Value ll_bshared_select_dynamic(Value target, Value name_val);

/* `expr.${nameExpr} or default` (M28) -- see the full comment in
 * builtins_shared.c. Returns 1 and writes the forced value through
 * `*out` on a hit, 0 (not a non-attrset/missing-key fatal, unlike the
 * plain form above) on a miss -- a non-string name is still fatal. */
int ll_bshared_select_dynamic_or(Value target, Value name_val, Value *out);

/* `expr ? ${nameExpr}` (M33, OP_HAS_ATTR_DYNAMIC -- see ir.h). Membership-
 * only, never fatal on a non-attrset target (returns 0, matching plain
 * OP_HAS_ATTR's graceful "no") -- a non-string name is still fatal, same
 * rule as every other dynamic-attr-name primitive. Never forces the
 * matched entry's value. */
int ll_bshared_has_attr_dynamic(Value target, Value name_val);

/* M23 originally added `ll_bshared_make_lazy_call1`/`_call2` here --
 * a genuinely LAZY Thunk+Env pair that, when first forced, computed `f
 * arg` or the curried `(f arg1) arg2` instead of an eager ll_apply
 * call made right away (fixing a real, previously-undiscovered
 * strictness bug: `map`/`mapAttrs` were calling their function argument
 * immediately, once per element, while building their result --
 * confirmed: `builtins.length (map (x: throw "boom") [1 2 3])` => 3 on
 * real Nix, aborted here; numtide/nix-conformance's eval-okay-
 * intersectAttrs.nix relies on exactly this laziness via `builtins.
 * mapAttrs throw alphabet` as a "canary" that must NOT fire just from
 * being built). M109 later added a mapAttrs/zipAttrsWith-specific
 * shared-Env variant (`ll_bshared_make_call2_shared_env`/
 * `_make_lazy_call2_indexed`) to avoid re-storing `f` into a fresh Env
 * per output element. M117 replaced ALL of the above with
 * `ll_make_lazyapp1`/`ll_make_lazyapp2` (value_util.h) -- same
 * laziness guarantee, same callers (map/mapAttrs/zipAttrsWith), but a
 * single lightweight LazyApp heap object instead of any Env+Thunk pair
 * (shared-Env or not) per output element (see heap.h's
 * LL_LAZYAPP_MARKER comment for the full rationale) -- removed here
 * as dead code once their last callers were ported. */

#endif /* LL_BUILTINS_SHARED_H */
