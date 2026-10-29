/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_BUILTINS_SHARED_H
#define LL_BUILTINS_SHARED_H

#include <stddef.h>
#include <stdint.h>

#include "ll/env.h"
#include "ll/value.h"
#include "builtins_list.h"

Value ll_bshared_make_primop(void *fn, int64_t arity);

/* Same as ll_bshared_make_primop, except the resulting PrimOp's
 * arguments are NOT pre-forced by op_call.S before the native function
 * is called (LL_PRIMOP_OFF_NOFORCE, heap.h) -- used only by
 * `tryEval`, which needs its raw, unforced argument so it can
 * force it itself inside its own setjmp/longjmp catch scope. */
Value ll_bshared_make_primop_noforce(void *fn, int64_t arity);

Value ll_bshared_make_curried_native(void *native_fn, int64_t arity);

/* Same curry-chain shape as ll_bshared_make_curried_native just above,
 * EXCEPT the generated chain reassembles its captured arguments with
 * OP_VAR_REF_RAW instead of plain OP_VAR_REF, and the leaf native call
 * is built via ll_bshared_make_primop_noforce instead of
 * ll_bshared_make_primop -- so NEITHER the curry chain's own reassembly
 * NOR op_call.S's post-dispatch primop-arg-forcing loop ever forces any
 * argument before `native_fn` runs; `native_fn` is entirely responsible
 * for forcing whatever it needs itself. Needed by `elem`
 * (builtins_list2.c): its first argument must reach ll_bshared_value_eq
 * completely raw/unforced, so that function's own slot/Cell-identity
 * shortcut can still fire -- forcing it first (what
 * ll_bshared_make_curried_native's plain OP_VAR_REF reassembly does,
 * harmlessly for every OTHER curried builtin, since op_call.S's own
 * force-loop would force it right back anyway) throws away exactly that
 * identity information before `elem`'s own body ever sees it (confirmed
 * via numtide/nix-conformance's eval-okay-eq-slot-elem.nix). Safe to use
 * for OTHER arguments too (as `elem`'s own `list` argument does) as long
 * as the native function forces every argument it actually needs the
 * content of -- same discipline tryEval's own
 * ll_bshared_make_primop_noforce already requires of its one caller. */
Value ll_bshared_make_curried_native_noforce(void *native_fn, int64_t arity);

/* A single, lazily-allocated, byte-correct empty-attrset Value (count=0),
 * shared by every curry-stage Closure's OFF_FORMALS field -- see main.c's
 * LL_TAG_CLOSURE print case for why this needs to be ONE canonical address
 * rather than each call site's own fresh `ll_bshared_make_attrs(NULL,
 * NULL, 0)` (`builtins.functionArgs` on any of these closures is
 * unaffected, still a genuinely valid, correct `{ }`). The only purpose
 * this adds is a RELIABLE, pointer-identity-based way for the printer to
 * recognize "this Closure is a native-builtin curry stage, not user Nix
 * code": compiler.c's own AST_LAMBDA case calls the plain
 * `ll_bshared_make_attrs(NULL, NULL, 0)` fresh for every single compiled
 * non-pattern lambda (never this function), so a real user lambda's
 * formals pointer can never coincide with this one, no matter how many
 * are compiled. */
Value ll_bshared_native_formals_marker(void);

Value ll_bshared_apply2(Value fn, Value a, Value b);

void ll_bshared_fatal(const char *who, const char *msg);
_Noreturn void ll_bshared_select_missing_fatal(uint32_t sym);

/* Returns a static, non-owned "a/an <kind>" string
 * (int/bool/null/string/path/list/attrset/function/...), the same
 * tag -> name mapping builtins.c's `prim_typeOf` already established.
 * Non-static (unlike most one-off helpers in this file) because
 * attrs_update.c's ll_attrs_insert_dynamic needs it too. */
const char *ll_bshared_type_name(Value v);

_Noreturn void ll_bshared_select_type_fatal(Value v);

_Noreturn void ll_bshared_call_type_fatal(Value v);

/* `__functor` protocol -- see the full comment in builtins_shared.c.
 * Called from both OP_CALL (op_call.S) and ll_apply (apply.S) only
 * after their own Closure/PrimOp dispatch has already failed to match
 * an attrset with a `__functor` attribute. Fatal (reusing
 * ll_bshared_call_type_fatal) if `__functor` is absent. */
Value ll_bshared_call_functor(Value attrs_val, Value arg);

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

/* Bounded multi-leaf probe, no materialization -- see builtins_shared.c. */
int ll_bshared_attrs_probe(Value attrs, uint32_t sym, Value *out_value);

/* `v` MUST already be LL_TAG_ATTRS (callers check that first, same as
 * every other attrs_* accessor here). Returns `v` unchanged if it's
 * already an ordinary flat Attrs. If `v` is instead a UnionHdr,
 * materializes its fully flattened, deduped, sym-sorted equivalent on
 * first call and caches the result in the UnionHdr's own `flat_cache`
 * slot -- every later call on the SAME UnionHdr Value just returns the
 * cached Value, no re-merge. EVERY raw-layout reader of an ATTRS object
 * (every op_*.S binary search, this file's own count/sym/value/find
 * below, derivation.c, main.c's printer, builtins_attrs.c,
 * builtins_m54.c's unsafeGetAttrPos) MUST call this first and use only
 * ITS result -- reading the raw fields of a Value that might still be a
 * UnionHdr without resolving first is a correctness bug, not just a
 * missed optimization. */
Value ll_bshared_attrs_resolve(Value v);

/* `syms`/`vals` need NOT be pre-sorted (sorted in place, ascending by sym
 * id, before building) -- unlike builtins_attrs.c's make_attrs_from_sorted
 * (which only ever inherits sortedness from an existing Attrs object). */
Value ll_bshared_make_attrs(uint32_t *syms, Value *vals, int64_t count);

/* `rec { ... __overrides = {...}; ... }` (OP_REC_OVERRIDES_MERGE -- see
 * that op's long comment, ir.h, for the full picture). `env` is the
 * rec's own just-OP_FRAME_ALLOC'd Env (`rdi` at op-handler entry); `n` and
 * `meta` are the op's `a`/pool-at-`b` operands verbatim and MUST match
 * that layout exactly: `meta[0..n-1]` are the n static bindings' symbol
 * ids in original (= Env slot) order, `meta[n]` is which of those n slots
 * holds `__overrides` itself. Forces `env->values[meta[n]]`, requires
 * it's an Attrs (fatal otherwise), merges it into the n static (sym,
 * env->values[i]) pairs -- matched syms overwrite BOTH the output pair's
 * value AND env->values[i] itself (so sibling bindings referencing that
 * slot see the override), unmatched override keys are appended as extra
 * output-only pairs -- and returns the one finished, freshly-sorted
 * Attrs Value OP_REC_OVERRIDES_MERGE pushes. */
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
 * thunk found along the way via the already-existing `ll_force_for_print`
 * C-callable force entry point (`ll/ir.h`), safe to call re-entrantly
 * here. Closures/primops fall back to pointer identity -- real Nix never
 * structurally compares functions either (confirmed: two syntactically-
 * identical-but-distinct lambdas are `!=`, see
 * tests/conformance/equal-function-*.nix). This is the single
 * implementation both the `==`/`!=` operators (op_cmp.S) and dedup-style
 * builtins (genericClosure, elem) use. NOTE: a prior version of this
 * function fell back to pointer identity for lists/attrsets too -- that
 * was a real, high-impact bug (not an acceptable scope cut), since `==`
 * on aggregates is pervasive in real nixpkgs code; do not reintroduce a
 * pointer-identity shortcut for the list/attrset cases above. */
int ll_bshared_value_eq(Value a, Value b);

/* The `==`/`!=` OPERATORS' OWN two direct operands need one more rule on
 * top of ll_bshared_value_eq above: real Nix's `ExprOpEq::eval` always
 * evaluates each of its two operands into a *fresh* local `Value` before
 * calling `eqValues`, so `eqValues`'s own pointer-equality hack can NEVER
 * fire for `==`'s own two top-level operands, no matter what expression
 * either side is -- confirmed against real Nix: `let f = x: x; in f == f`
 * is `false`, even though both sides are literally the same binding. That
 * pointer-identity hack DOES still fire one level down inside a
 * list/attrset (`[f] == [f]` is `true`). This wrapper only strengthens
 * the TOP-level, direct-operand case: if both forced operands are the
 * same function-ish tag (closure or primop), the answer is
 * unconditionally "not equal", matching ExprOpEq::eval exactly;
 * everything else defers entirely to ll_bshared_value_eq. Do not replace
 * callers of this with a direct call to ll_bshared_value_eq -- that
 * reintroduces the `f == f` bug above. */
int ll_bshared_value_eq_op(Value a, Value b);

/* Ordering used by lessThan/sort: INT<INT (numeric), string<string
 * (lexicographic byte compare, shorter-is-less on a shared prefix), and
 * list<list (lexicographic element-by-element, recursing this same
 * function on the first differing pair -- shorter-is-less on an equal
 * common prefix). Narrower than real Nix's `lessThan` (which also orders
 * paths). Fatal on any other type, or a type mismatch. */
int ll_bshared_value_lt(Value a, Value b, const char *who);

/* Runtime implementation of `expr.${nameExpr}` (dynamic attribute select,
 * OP_SELECT_DYNAMIC -- see ir.h and op_select_dynamic.S). Fatal on a
 * non-attrset target, a non-string name, or a missing key (same error
 * class as plain OP_SELECT), unlike OP_SELECT_OR's graceful fallthrough.
 * Forces the result if it's an unforced thunk, matching plain
 * OP_SELECT's laziness. */
Value ll_bshared_select_dynamic(Value target, Value name_val);

/* `expr.${nameExpr} or default` -- see the full comment in
 * builtins_shared.c. Returns 1 and writes the forced value through
 * `*out` on a hit, 0 (not a non-attrset/missing-key fatal, unlike the
 * plain form above) on a miss -- a non-string name is still fatal. */
int ll_bshared_select_dynamic_or(Value target, Value name_val, Value *out);

/* `expr ? ${nameExpr}` (OP_HAS_ATTR_DYNAMIC -- see ir.h). Membership-only,
 * never fatal on a non-attrset target (returns 0, matching plain
 * OP_HAS_ATTR's graceful "no") -- a non-string name is still fatal. Never
 * forces the matched entry's value. */
int ll_bshared_has_attr_dynamic(Value target, Value name_val);

#endif /* LL_BUILTINS_SHARED_H */
