/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_VALUE_UTIL_H
#define LL_VALUE_UTIL_H

#include <stddef.h>
#include <stdint.h>

#include "ll/heap.h"
#include "ll/value.h"

Value ll_make_string_value(const char *s, size_t len);

/* `abs_path` must already be an absolute, canonicalized path -- this
 * helper does no resolution of its own. */
Value ll_make_path_value(const char *abs_path, size_t len);

/* Must be LL_PTR_BASE_MASK (clears the ThunkMap index bits too), never
 * the narrower ~0x7ULL, even though none of Path/Float/Cell is ever
 * itself a ThunkMap -- a caller may hand any of these a value that
 * turns out to be one (using the narrower mask caused a real
 * ll_value_is_cell segfault). */
#define LL_PTR_BASE_MASK 0x0000FFFFFFFFFFF8ULL

static inline __attribute__((always_inline)) int ll_value_is_path(Value v) {
    if ((v.raw & LL_TAG_MASK) != LL_TAG_PTR) return 0;
    const char *base = (const char *)(v.raw & LL_PTR_BASE_MASK);
    return *(const int64_t *)(base + LL_PATH_OFF_MARKER) == LL_PATH_MARKER;
}

Value ll_make_float_value(double d);

static inline __attribute__((always_inline)) int ll_value_is_float(Value v) {
    if ((v.raw & LL_TAG_MASK) != LL_TAG_PTR) return 0;
    const char *base = (const char *)(v.raw & LL_PTR_BASE_MASK);
    return *(const int64_t *)(base + LL_FLOAT_OFF_MARKER) == LL_FLOAT_MARKER;
}

/* Aborts if `v` isn't a Float -- callers must ll_value_is_float() first. */
double ll_float_value(Value v);

/* `target` must be a stable (arena-allocated, never freed) Value* --
 * in practice always `&env->values[slot]` for some live Env. */
Value ll_make_cell_value(Value *target);

static inline __attribute__((always_inline)) int ll_value_is_cell(Value v) {
    if ((v.raw & LL_TAG_MASK) != LL_TAG_PTR) return 0;
    const char *base = (const char *)(v.raw & LL_PTR_BASE_MASK);
    return *(const int64_t *)(base + LL_CELL_OFF_MARKER) == LL_CELL_MARKER;
}

/* Aborts if `v` isn't a Cell -- callers must ll_value_is_cell() first. */
Value *ll_cell_target(Value v);

/* The shared "unwrap however many Cells are chained together" loop,
 * called from each force chokepoint once it's already established `v`
 * IS a Cell. A Cell has no IN_PROGRESS/blackhole state of its own (it's
 * just {marker; target}), so a pure alias cycle with no Thunk anywhere
 * in it (e.g. `let { x = y; y = x; body = x; }`) would otherwise chase
 * Cell->Cell->Cell forever -- this was a real regression (hung instead
 * of reporting the expected "blackhole" error) once Cells could alias
 * each other. Aborts with the same wording the Thunk-based blackhole
 * check uses once the chain exceeds LL_CELL_CHASE_LIMIT. Returns the
 * first non-Cell Value found (which may still be an unforced Thunk --
 * the caller's own resolve loop handles that). */
Value ll_resolve_cell_chain(Value v);

Value ll_make_lazyapp1(Value fn, Value arg1);
Value ll_make_lazyapp2(Value fn, Value arg1, Value arg2);

int ll_value_is_lazyapp(Value v);

/* Performs exactly ONE step (compute-and-memoize-if-unresolved, or read
 * back the cached result if already resolved) rather than looping
 * internally -- the result may itself still be an unforced
 * Thunk/Cell/LazyApp, so the caller's own resolve loop must re-dispatch
 * on it, same as it already does for ll_resolve_cell_chain's return
 * value. */
Value ll_resolve_lazyapp(Value v);

/* ll_thunkmap_make_desc allocates and partially initializes the
 * descriptor (marker/f/src_entries); `out` is left as the "never a
 * legal Value" raw-0 sentinel until ll_thunkmap_set_out patches it in,
 * since the caller can only build `out` itself after every element's
 * packed word (which needs the descriptor's address, but not `out`'s)
 * already exists. Returns the raw descriptor pointer (not a tagged
 * Value) -- callers need the bare address both to pack elements and to
 * patch `out` in after. */
void *ll_thunkmap_make_desc(Value f, const void *src_entries);

/* Packs element `idx` of `desc`'s call into one LL_TAG_PTR Value. `idx`
 * must be <= LL_THUNKMAP_MAX_IDX (the caller is responsible for falling
 * back to the per-element LazyApp path above that bound). */
Value ll_thunkmap_pack(void *desc, int64_t idx);

void ll_thunkmap_set_out(void *desc, Value out);

int ll_value_is_thunkmap(Value v);

/* The shared "run it (or return the already-cached result)" step for a
 * ThunkMap element. Same one-step contract as ll_resolve_lazyapp --
 * may hand back another not-yet-forced Thunk/Cell/LazyApp/ThunkMap for
 * the caller's own resolve loop to keep unwrapping. */
Value ll_resolve_thunkmap(Value v);

void *ll_formalsmap_make_desc(Value arg, const int64_t *syms, void *env);
Value ll_formalsmap_pack(void *desc, int64_t idx);
int ll_value_is_formalsmap(Value v);
Value *ll_formalsmap_slot(Value v);
Value ll_resolve_formalsmap(Value v);

/* The ancestor-cycle-detection key main.c's printer needs for a
 * ThunkMap word: a pointer to the SPECIFIC output slot this element's
 * result will be (or already was) memoized into -- unique per (desc,
 * idx) pair, unlike `v`'s own masked pointer (which names the shared
 * descriptor, identical across every element of one call, and would
 * misfire on unrelated sibling elements if used as the ancestor key
 * instead). */
Value *ll_thunkmap_slot(Value v);

/* Returns a pointer to the raw bytes (NOT NUL-terminated either way)
 * and length of an LL_TAG_PTR value, whichever kind (String or Path) it
 * actually is. Aborts if `v` isn't LL_TAG_PTR. */
const char *ll_ptr_bytes(Value v, int64_t *out_len);

/* Coerces INT/BOOL/NULL/string/path to a string Value, matching real
 * Nix's `toString`. Aborts the process on an uncoercible type
 * (list/attrset/closure/etc). */
Value ll_coerce_to_string(Value v);

/* `derivation {...}`'s own attribute coercion -- same permissiveness
 * as `ll_coerce_to_string` but, unlike it, a Path NOT already under
 * `/nix/store/` is NAR-hashed and copied into the store instead of
 * being left as plain literal text. */
Value ll_coerce_to_string_for_derivation(Value v);

/* `builtins.concatStringsSep`'s per-element coercion -- same rules as
 * genuine `"${...}"` string interpolation (strings, paths -- copied to
 * the store if not already there --, and attrsets with `__toString`/
 * `outPath`; ints/bools/null/lists are rejected, unlike
 * `ll_coerce_to_string`/`toString`). */
Value ll_coerce_to_string_for_concat_sep(Value v);

/* The runtime half of string interpolation ("foo ${expr} bar"): pops
 * `count` parts pushed on VALSTACK and concatenates them into one
 * freshly arena-allocated String. Each non-string part is rejected with
 * a fatal error, NOT coerced the way `ll_coerce_to_string`/
 * `builtins.toString` would -- this matches real Nix exactly (`"${7}"`
 * fails even though `toString 7` succeeds). An interpolated non-string
 * value needs an explicit `builtins.toString` in the source. */
Value ll_concat_strings(const Value *parts, int64_t count);

/* The runtime half of `~`/path-literal interpolation (`./${foo}`,
 * `~/${foo}`, `/${foo}`, ...). Identical to ll_concat_strings except the
 * result is re-canonicalized and rewrapped as a Path afterward. Only
 * ever called with `parts[0]` already a Path constant. */
Value ll_concat_path(const Value *parts, int64_t count);

/* The `+` operator's full dispatch, not just integer arithmetic -- real
 * Nix's `+` does int addition for two ints, string/path concatenation
 * for anything else, and is a hard type error if exactly one side is an
 * int. Result stays a Path (rather than becoming a plain String) only
 * when the LHS specifically was a Path -- mixed string+path or
 * path+path beyond that single rule is a documented, narrower scope cut
 * (real Nix additionally runs a path-existence check and copies-to-store
 * in some of these mixed cases -- not replicated here). */
Value ll_add_values(Value lhs, Value rhs);

/* Lexically canonicalizes an absolute path (collapses `.`/`..`
 * components and doubled slashes, same as real Nix's `canonPath`) --
 * never touches the filesystem, no symlink resolution, just text. `in`
 * must start with '/'. Returns a freshly malloc'd, NUL-terminated
 * string the caller owns. */
char *ll_canon_path(const char *in);

/* `-`/`*`/`/`'s slow-path fallback: only called when the all-int fast
 * path didn't match -- i.e. at least one operand isn't a plain int.
 * Handles every int/float and float/float combination (promoting an int
 * operand to double, matching real Nix's own mixed-arithmetic promotion
 * rule), fatal for anything else. Division additionally fatals on a
 * zero divisor the same way the all-int fast path already does -- even
 * `1.0 / 0.0` is a hard "division by zero" error, not an IEEE-754
 * infinity, matching real Nix. */
Value ll_sub_values(Value lhs, Value rhs);
Value ll_mul_values(Value lhs, Value rhs);
Value ll_div_values(Value lhs, Value rhs);

/* `<`/`>`/`<=`/`>=`'s slow-path fallback: only called when the all-int
 * fast path didn't match. Each returns an already-tagged LL_TRUE/
 * LL_FALSE Value (not a plain C bool) -- the asm callers store the
 * result straight to VALSTACK. Promotes int<->float the same way the
 * arithmetic fallbacks above do; fatal for any operand that isn't
 * numeric. A NaN operand falls out correctly for free: a plain C
 * `<`/`>`/`<=`/`>=` on a NaN double is false by IEEE-754 definition,
 * matching real Nix. */
Value ll_cmp_lt_values(Value lhs, Value rhs);
Value ll_cmp_gt_values(Value lhs, Value rhs);
Value ll_cmp_le_values(Value lhs, Value rhs);
Value ll_cmp_ge_values(Value lhs, Value rhs);

/* The `++` operator (list concatenation) -- NOT the same as `+`, which
 * has no list case at all in real Nix (`[1] + [2]` is a type error
 * there). Fatal if either side isn't a list. */
Value ll_concat_lists(Value lhs, Value rhs);

/* Arena-allocates a NUL-terminated copy of an asnix String's bytes.
 * asnix strings are never NUL-terminated themselves; code boundaries
 * that expect plain C strings (aterm.c's API) need one. */
char *ll_string_value_to_cstr(Value string_val);

#endif /* LL_VALUE_UTIL_H */
