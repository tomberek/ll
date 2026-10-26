/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef ASNIX_VALUE_UTIL_H
#define ASNIX_VALUE_UTIL_H

#include <stddef.h>
#include <stdint.h>

#include "asnix/value.h"

/* Small, shared helpers used by multiple builtins (builtins.c, derivation.c)
 * -- factored out once a second user appeared, not speculatively. */

/* Arena-allocates a String{len;bytes} heap object and returns it tagged. */
Value asnix_make_string_value(const char *s, size_t len);

/* Arena-allocates a Path heap object (see heap.h) and returns it tagged
 * ASNIX_TAG_PTR. `abs_path` must already be an absolute, canonicalized
 * path (see compiler.c's AST_PATH case) -- this helper does no resolution
 * of its own. */
Value asnix_make_path_value(const char *abs_path, size_t len);

/* True iff `v` is an ASNIX_TAG_PTR object storing a Path rather than a
 * String (see heap.h's ASNIX_PATH_MARKER note). False for any other tag. */
int asnix_value_is_path(Value v);

/* Float (M51): a third ASNIX_TAG_PTR kind alongside String/Path -- see
 * heap.h's ASNIX_FLOAT_MARKER note for the full rationale. Arena-allocates
 * a Float{marker;value} heap object and returns it tagged ASNIX_TAG_PTR. */
Value asnix_make_float_value(double d);

/* True iff `v` is an ASNIX_TAG_PTR object storing a Float (see heap.h's
 * ASNIX_FLOAT_MARKER note). False for any other tag, including String and
 * Path -- callers that already know `v` is ASNIX_TAG_PTR and need "is this
 * specifically a string" should check `!asnix_value_is_path(v) &&
 * !asnix_value_is_float(v)`. */
int asnix_value_is_float(Value v);

/* Unboxes a Float Value's double payload. Aborts if `v` isn't one (same
 * "trust the caller already checked the tag" convention as
 * asnix_int_value) -- callers must asnix_value_is_float() first. */
double asnix_float_value(Value v);

/* Cell (M92): see heap.h's ASNIX_CELL_MARKER comment for the full
 * rationale -- a purely-internal "alias this existing Env slot instead of
 * allocating a fresh Thunk" device, used by compiler.c's `try_cell_ref`
 * at each of the ten `ib_mk_thunk` call sites when the expression being
 * thunked is a bare, lexically-resolved variable reference. Arena-
 * allocates a Cell{marker;target} heap object and returns it tagged
 * ASNIX_TAG_PTR. `target` must be a stable (arena-allocated, never freed)
 * Value* -- in practice always `&env->values[slot]` for some live Env
 * (src/asm/ops/op_var_ref_cell.S is the only caller). */
Value asnix_make_cell_value(Value *target);

/* True iff `v` is an ASNIX_TAG_PTR object storing a Cell rather than a
 * String/Path/Float (see heap.h's ASNIX_CELL_MARKER note). False for any
 * other tag. */
int asnix_value_is_cell(Value v);

/* Unboxes a Cell Value's target pointer. Aborts if `v` isn't one (same
 * "trust the caller already checked the tag" convention as
 * asnix_int_value/asnix_float_value) -- callers must asnix_value_is_cell()
 * first. */
Value *asnix_cell_target(Value v);

/* M92: the single, shared "unwrap however many Cells are chained
 * together" loop, called from each force chokepoint (force.S,
 * op_var_ref.S, op_select.S, op_select_or.S, op_with.S's OP_WITH_TRY)
 * once it's already established `v` IS a Cell -- centralizing the loop
 * here (rather than inlining an unbounded dereference-and-jump-back-to-
 * resolve in all five places) is what makes it practical to bound it:
 * a Cell has no IN_PROGRESS/blackhole state of its own the way a Thunk
 * does (see heap.h's ASNIX_CELL_MARKER comment -- it's just {marker;
 * target}, deliberately lightweight), so a PURE alias cycle with no
 * Thunk anywhere in it at all (e.g. the legacy-`let`-syntax mutual
 * reference `let { x = y; y = x; body = x; }`) would otherwise chase
 * Cell->Cell->Cell forever with nothing to ever notice the cycle and
 * stop -- found as a real regression (numtide/nix-conformance's
 * eval-fail-blackhole.nix hanging instead of reporting its expected
 * "blackhole" error) once Cells started being able to alias EACH
 * OTHER, not just a ground Thunk/concrete value. Aborts with the exact
 * same wording the Thunk-based blackhole check already uses once the
 * chain exceeds ASNIX_CELL_CHASE_LIMIT (chosen generously above any
 * legitimate chain depth -- a real program's own bare-variable-alias
 * chain is never more than a few hops deep -- while still aborting in a
 * small, bounded amount of time on a genuine cycle). Returns the first
 * non-Cell Value found (which may still be an unforced Thunk -- the
 * caller's own existing thunk-forcing logic handles that, by jumping
 * back to its own resolve loop once this returns, exactly as it would
 * for any other non-Cell value). */
Value asnix_resolve_cell_chain(Value v);

/* LazyApp (M113): see heap.h's ASNIX_LAZYAPP_MARKER comment for the full
 * rationale -- a purely-internal "run `fn arg1[ arg2]` on demand,
 * without allocating an Env or a Thunk" device, used by `map`
 * (builtins_list.c)/`mapAttrs` (builtins_attrs.c)/`zipAttrsWith`
 * (builtins_m54.c) in place of the old asnix_bshared_make_lazy_call1/
 * _call2. `asnix_make_lazyapp1` is `map`'s single-argument `fn arg1`
 * case; `asnix_make_lazyapp2` is the curried `(fn arg1) arg2` case
 * (`fn name value`) `mapAttrs`/`zipAttrsWith` need. */
Value asnix_make_lazyapp1(Value fn, Value arg1);
Value asnix_make_lazyapp2(Value fn, Value arg1, Value arg2);

/* True iff `v` is an ASNIX_TAG_PTR object storing a LazyApp (either
 * sub-state -- see heap.h's ASNIX_LAZYAPP_MARKER/ASNIX_LAZYAPP_FORCED_
 * MARKER). False for any other tag, including String/Path/Float/Cell. */
int asnix_value_is_lazyapp(Value v);

/* The shared "run it (or return the already-cached result)" step,
 * called from each force chokepoint (force.S, op_var_ref.S,
 * op_select.S, op_select_or.S, op_with.S's OP_WITH_TRY) once it's
 * already established `v` IS a LazyApp (either marker). Mirrors
 * asnix_resolve_cell_chain's role for Cell, with one difference: this
 * does exactly ONE step (compute-and-memoize-if-unresolved, or just
 * read back the cached result if already resolved) rather than
 * looping internally -- the result may itself still be an unforced
 * Thunk/Cell/LazyApp (the same "lazy native primop passthrough" case
 * M23's force.S comment already documents for Thunk), so the CALLER's
 * own resolve loop is what re-dispatches on it, exactly as it already
 * does for asnix_resolve_cell_chain's own return value. See heap.h's
 * own comment for why this deliberately has no bespoke IN_PROGRESS/
 * blackhole or FAILED/memoized-failure handling the way Thunk's
 * four-state machine does. */
Value asnix_resolve_lazyapp(Value v);

/* ThunkMap (M118): see heap.h's ASNIX_THUNKMAP_MARKER comment for the full
 * rationale/layout -- replaces LazyApp specifically for `mapAttrs`
 * (builtins_attrs.c)/`zipAttrsWith` (builtins_m54.c), one shared
 * descriptor per CALL instead of one heap object per output element.
 *
 * asnix_thunkmap_make_desc allocates and partially initializes the
 * descriptor (marker/f/src_entries); `out` is left as the "never a legal
 * Value" raw-0 sentinel until asnix_thunkmap_set_out patches it in, since
 * the caller can only build `out` itself (make_attrs_from_sorted/
 * asnix_bshared_make_attrs) AFTER every element's packed word (which
 * needs the descriptor's address, but not `out`'s) already exists.
 * Returns the raw descriptor pointer (not a tagged Value -- callers need
 * the bare address both to pack elements and to patch `out` in after). */
void *asnix_thunkmap_make_desc(Value f, const void *src_entries);

/* Packs element `idx` of `desc`'s call into one ASNIX_TAG_PTR Value --
 * this is what callers store directly into the output attrset's own
 * entries[idx].value slot. `idx` must be <= ASNIX_THUNKMAP_MAX_IDX (the
 * caller is responsible for falling back to the old per-element LazyApp
 * path above that bound -- see heap.h's comment). */
Value asnix_thunkmap_pack(void *desc, int64_t idx);

/* Patches `out` into an already-allocated descriptor, once the caller has
 * finished building it (see asnix_thunkmap_make_desc's own comment for
 * why this has to happen in a separate, later step). */
void asnix_thunkmap_set_out(void *desc, Value out);

/* True iff `v` is an ASNIX_TAG_PTR value storing a packed ThunkMap word
 * (see heap.h's ASNIX_THUNKMAP_MARKER note). False for any other tag,
 * including String/Path/Float/Cell/LazyApp. */
int asnix_value_is_thunkmap(Value v);

/* The shared "run it (or return the already-cached result)" step for a
 * ThunkMap element, called from each force chokepoint (force.S,
 * op_var_ref.S, op_select.S, op_select_or.S, op_with.S's OP_WITH_TRY) and
 * from main.c's printer once it's already established `v` IS a ThunkMap
 * word. Same "exactly one step, may hand back another not-yet-forced
 * Thunk/Cell/LazyApp/ThunkMap for the caller's own resolve loop to keep
 * unwrapping" contract as asnix_resolve_lazyapp -- see that function's own
 * comment, and heap.h's ASNIX_THUNKMAP_MARKER comment for the full
 * memoization proof. */
Value asnix_resolve_thunkmap(Value v);

/* ll-only, no asnix equivalent: see heap.h's ASNIX_FORMALSMAP_MARKER
 * comment for the full design -- mirrors the four ThunkMap functions
 * just above, for a different per-call-shared computation (binding a
 * pattern-lambda's required formals without one Thunk per formal).
 * Measured on ffmpeg-full.outPath: -3.42% instructions (callgrind),
 * -5.06% instructions / -27.5% wall clock (pinned perf), -11.5% peak
 * RSS, -10.9% minor page faults, -8% cache misses, vs. the prior
 * one-Thunk-per-required-formal baseline -- see M8-findings.md. */
void *asnix_formalsmap_make_desc(Value arg, const int64_t *syms, void *env);
Value asnix_formalsmap_pack(void *desc, int64_t idx);
int asnix_value_is_formalsmap(Value v);
Value *asnix_formalsmap_slot(Value v);
Value asnix_resolve_formalsmap(Value v);

/* The ancestor-cycle-detection key main.c's printer needs for a ThunkMap
 * word: a pointer to the SPECIFIC output slot this element's result will
 * be (or already was) memoized into -- unique per (desc, idx) pair, unlike
 * `v`'s own masked pointer (which names the shared descriptor, identical
 * across every element of one call -- see heap.h's comment for why using
 * THAT as the ancestor key would misfire on unrelated sibling elements). */
Value *asnix_thunkmap_slot(Value v);

/* Returns a pointer to the raw bytes (NOT NUL-terminated either way) and
 * length of an ASNIX_TAG_PTR value, whichever of the two kinds (String or
 * Path) it actually is -- for callers (printing, hashing, ...) that just
 * want "the text" regardless of which. Aborts if `v` isn't ASNIX_TAG_PTR. */
const char *asnix_ptr_bytes(Value v, int64_t *out_len);

/* Coerces INT/BOOL/NULL/string/path to a string Value, matching real
 * Nix's `toString` (this *is* `toString`'s logic -- `prim_toString` in
 * builtins.c just calls this directly). A Path argument is converted to a
 * plain String of its (already-absolute) text -- matching real Nix, where
 * `toString somePath` changes the value's type from "path" to "string"
 * (verified against `nix-instantiate --eval`; this project does not model
 * string "context", so unlike real Nix this never copies anything to the
 * store -- see docs/architecture.md's import/path scope notes). Aborts
 * the process on an uncoercible type (list/attrset/closure/etc), same as
 * real Nix. */
Value asnix_coerce_to_string(Value v);

/* M75: `derivation {...}`'s own attribute coercion -- same
 * permissiveness as `asnix_coerce_to_string` (int/bool/null/list/attrs
 * with `__toString`/`outPath` all accepted) but, unlike it, a Path NOT
 * already under `/nix/store/` is NAR-hashed and copied into the store
 * (matching real Nix's `coerceToString(..., copyToStore=true)`,
 * `~/nix/src/libexpr/primops.cc`'s `derivationStrictInternal`) instead
 * of being left as plain literal text. See value_util.c's own comment
 * for why neither `asnix_coerce_to_string` nor the (separate, stricter)
 * string-interpolation coercion alone provide this combination. */
Value asnix_coerce_to_string_for_derivation(Value v);

/* `builtins.concatStringsSep`'s per-element coercion -- same rules as
 * genuine `"${...}"` string interpolation (strings, paths -- copied to
 * the store if not already there --, and attrsets with `__toString`/
 * `outPath`; ints/bools/null/lists are rejected, unlike
 * `asnix_coerce_to_string`/`toString`). See value_util.c's own comment
 * on this function for why concatStringsSep needs this exact subset
 * rather than either existing coercion function's rules. */
Value asnix_coerce_to_string_for_concat_sep(Value v);

/* Concatenates `count` values into one freshly arena-allocated String --
 * the runtime half of string interpolation ("foo ${expr} bar"): the
 * compiler pushes each literal chunk (already a string constant) and each
 * interpolated expression's raw result, then OP_STR_CONCAT (src/asm/ops/
 * op_str_concat.S) calls this with a pointer straight into VALSTACK, same
 * pop-N-build-one shape as OP_MK_LIST/OP_MK_ATTRS.
 *
 * Each non-string part is rejected with a fatal error, NOT coerced the way
 * `asnix_coerce_to_string`/`builtins.toString` would -- this matches real
 * Nix exactly (verified with `nix-instantiate --eval`: `"${7}"` and
 * `"${true}"` and `"${null}"` all fail with "cannot coerce ... to a
 * string", even though `toString 7`/`true`/`null` succeed; real Nix's
 * `coerceToString` has a `coerceMore` flag that `toString` sets and plain
 * "${...}" interpolation doesn't). An interpolated non-string value needs
 * an explicit `builtins.toString` in the source, same as real Nix. */
Value asnix_concat_strings(const Value *parts, int64_t count);

/* M64: the runtime half of `~`/path-literal interpolation (`./${foo}`,
 * `~/${foo}`, `/${foo}`, ...) -- see OP_PATH_CONCAT in ir.h. Identical to
 * asnix_concat_strings except the result is re-canonicalized and
 * rewrapped as a Path afterward (matching real Nix's
 * `ExprConcatStrings::eval`, which always does this once the first
 * operand is a Path, regardless of whether that first operand's OWN
 * text was deliberately left un-canonicalized -- see compiler.c's
 * resolve_path_chunk). Only ever called with `parts[0]` already a Path
 * constant (compiler.c's AST_PATH case is the only caller, via
 * ib_path_concat/OP_PATH_CONCAT). */
Value asnix_concat_path(const Value *parts, int64_t count);

/* The `+` operator's full dispatch, not just integer arithmetic -- real
 * Nix's `+` does int addition for two ints, but string/path concatenation
 * (same coercion rules `asnix_concat_strings` already implements for
 * `"${a}${b}"` interpolation) for anything else, and is a hard type error
 * if exactly one side is an int (verified against `nix-instantiate
 * --eval`: `1 + "a"` -> "cannot add a string to an integer", `"a" + 1` ->
 * "cannot coerce an integer to a string" -- this project matches the
 * *class* of error, not real Nix's exact wording, same convention as every
 * other fatal message in this codebase). Result stays a Path (rather than
 * becoming a plain String) when the LHS specifically was a Path -- matches
 * the common real-world case (`somePath + "/subpath"`); mixed string+path
 * or path+path beyond that single rule is a documented, narrower scope cut
 * (real Nix additionally runs a path-existence check and copies-to-store
 * in some of these mixed cases -- not replicated here, see
 * docs/architecture.md). Used by `asnix_op_add` (src/asm/ops/op_arith.S)
 * whenever both operands aren't already known to be ints. */
Value asnix_add_values(Value lhs, Value rhs);

/* M57g: lexically canonicalizes an absolute path (collapses `.`/`..`
 * components and doubled slashes, same as real Nix's `canonPath` --
 * never touches the filesystem, no symlink resolution, just text).
 * `in` must start with '/'. Returns a freshly malloc'd, NUL-terminated
 * string the caller owns. Originally a compiler.c-only static helper
 * for canonicalizing path *literals* at compile time (`./a/../b`); M57g
 * reuses it in `asnix_add_values` too, so a `path + string`
 * concatenation's result gets the exact same canonicalization once it's
 * printed/`toString`-ed, matching real Nix (confirmed:
 * `toString (/foo/bar + "/../xyzzy/./foo.txt")` == `/foo/xyzzy/foo.txt`
 * in real Nix; asnix previously left `.`/`..` in literally, uncollapsed). */
char *asnix_canon_path(const char *in);

/* `-`/`*`/`/`'s slow-path fallback (M51): asnix_op_sub/mul/div
 * (src/asm/ops/op_arith.S) only call these when the all-int fast path
 * didn't match -- i.e. at least one operand isn't a plain int. Handles
 * every int/float and float/float combination (promoting an int operand
 * to double, matching real Nix's own mixed-arithmetic promotion rule --
 * verified against `nix-instantiate --eval`: `1 - 2.0`, `2.0 * 3`, `1 /
 * 2.0` all produce a float), fatal for anything else (matches the
 * existing int-only fatal's error *class*, not its exact wording, same
 * convention as every other fatal message in this codebase). Division
 * additionally fatals on a zero divisor the same way the all-int fast
 * path already does (checked against real Nix: even `1.0 / 0.0` is a
 * hard "division by zero" error there, not an IEEE-754 infinity --
 * confirmed directly, `~/nix/src/libexpr/primops.cc`'s `prim_div` checks
 * the float-coerced divisor against zero unconditionally, before ever
 * looking at whether either operand is actually a float). */
Value asnix_sub_values(Value lhs, Value rhs);
Value asnix_mul_values(Value lhs, Value rhs);
Value asnix_div_values(Value lhs, Value rhs);

/* `<`/`>`/`<=`/`>=`'s slow-path fallback (M51): asnix_op_lt/gt/le/ge
 * (src/asm/ops/op_cmp.S) only call these when the all-int fast path
 * didn't match. Each returns an already-tagged ASNIX_TRUE/ASNIX_FALSE
 * Value (not a plain C bool) -- same immediate-result convention the
 * asm callers already store straight to VALSTACK. Promotes int<->float
 * the same way the arithmetic fallbacks above do; fatal for any operand
 * that isn't numeric (int or float). A NaN operand falls out correctly
 * for free: a plain C `<`/`>`/`<=`/`>=` on a NaN double is false by
 * IEEE-754 definition, matching real Nix (which does the same plain C++
 * double comparison, `~/nix/src/libexpr/primops.cc`'s `CompareValues`). */
Value asnix_cmp_lt_values(Value lhs, Value rhs);
Value asnix_cmp_gt_values(Value lhs, Value rhs);
Value asnix_cmp_le_values(Value lhs, Value rhs);
Value asnix_cmp_ge_values(Value lhs, Value rhs);

/* The `++` operator (list concatenation) -- NOT the same as `+`, which has
 * no list case at all in real Nix (`[1] + [2]` is a type error there).
 * Fatal if either side isn't a list, matching real Nix's exact wording
 * (verified against `nix-instantiate --eval`: `1 ++ [2]` ->
 * "expected a list but found an integer: 1"). Used by `asnix_op_concat`
 * (src/asm/ops/op_concat.S). */
Value asnix_concat_lists(Value lhs, Value rhs);

/* Arena-allocates a NUL-terminated copy of an asnix String's bytes.
 * asnix strings are never NUL-terminated themselves (see heap.h), but
 * code boundaries that expect plain C strings (aterm.c's API) need one;
 * the arena is used (never freed) rather than malloc so nothing needs
 * explicit cleanup, consistent with this project's memory model. */
char *asnix_string_value_to_cstr(Value string_val);

#endif /* ASNIX_VALUE_UTIL_H */
