/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#define _DEFAULT_SOURCE /* lstat() -- same feature-test macro builtins_m23.c uses */

#include "value_util.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>

#include "ll/arena.h"
#include "ll/env.h" /* Env, for ll_resolve_formalsmap's writeback */
#include "ll/heap.h"
#include "ll/ir.h" /* ll_force_for_print, ll_apply */
#include "ll/stats.h"
#include "builtins_shared.h"
#include "nar.h"
#include "string_context.h"
#include "symtab.h"

/* Clears both the low 3 tag bits AND the high 16 index bits a packed
 * ThunkMap word (heap.h's LL_THUNKMAP_MARKER comment) carries in its
 * OWN 8-byte Value -- a no-op on every other LL_TAG_PTR kind (String/
 * Path/Float/Cell/LazyApp), whose own pointers always have zero there
 * already (see heap.h's comment), so EVERY function below that derives
 * a dereferenceable base pointer from an LL_TAG_PTR Value uses this
 * mask unconditionally, not just the ones that already know `v` is
 * specifically a ThunkMap -- using the narrower `~0x7ULL` mask anywhere
 * one of these functions might be handed an unresolved ThunkMap word
 * (e.g. ll_value_is_cell probing a value that turns out to be a
 * ThunkMap, main.c's printer walking through is_cell/is_lazyapp/
 * is_thunkmap in sequence) would leave the packed index bits in the
 * computed "base" pointer and crash dereferencing it -- confirmed: an
 * earlier version that only widened the mask at the force-chokepoint
 * Cell/LazyApp checks (not here) segfaulted in ll_value_is_cell on the
 * very first `mapAttrs` print.
 * NOTE: `ll_value_is_path`/`ll_value_is_float`/`ll_value_is_cell`
 * themselves moved to value_util.h as `static inline` -- this macro
 * moved with them; still used by every OTHER function below that
 * derives a base pointer the same way. */
#define LL_PTR_BASE_MASK 0x0000FFFFFFFFFFF8ULL

Value ll_make_string_value(const char *s, size_t len) {
    void *mem = ll_arena_alloc(8 + len);
    ll_stats_string++;
    *(int64_t *)((char *)mem + LL_STRING_OFF_LEN) = (int64_t)len;
    memcpy((char *)mem + LL_STRING_OFF_BYTES, s, len);
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | LL_TAG_PTR };
}

Value ll_make_path_value(const char *abs_path, size_t len) {
    void *mem = ll_arena_alloc(16 + len);
    ll_stats_path++;
    *(int64_t *)((char *)mem + LL_PATH_OFF_MARKER) = LL_PATH_MARKER;
    *(int64_t *)((char *)mem + LL_PATH_OFF_LEN) = (int64_t)len;
    memcpy((char *)mem + LL_PATH_OFF_BYTES, abs_path, len);
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | LL_TAG_PTR };
}

Value ll_make_float_value(double d) {
    void *mem = ll_arena_alloc(16);
    *(int64_t *)((char *)mem + LL_FLOAT_OFF_MARKER) = LL_FLOAT_MARKER;
    *(double *)((char *)mem + LL_FLOAT_OFF_VALUE) = d;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | LL_TAG_PTR };
}

double ll_float_value(Value v) {
    const char *base = (const char *)(v.raw & LL_PTR_BASE_MASK);
    return *(const double *)(base + LL_FLOAT_OFF_VALUE);
}

Value ll_make_cell_value(Value *target) {
    void *mem = ll_arena_alloc(16);
    ll_stats_cell++;
    *(int64_t *)((char *)mem + LL_CELL_OFF_MARKER) = LL_CELL_MARKER;
    *(Value **)((char *)mem + LL_CELL_OFF_TARGET) = target;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | LL_TAG_PTR };
}

Value *ll_cell_target(Value v) {
    const char *base = (const char *)(v.raw & LL_PTR_BASE_MASK);
    return *(Value **)(base + LL_CELL_OFF_TARGET);
}

/* LL_CELL_CHASE_LIMIT lives in heap.h -- shared with the hand-inlined
 * copies of this exact loop in op_var_ref.S/force.S so all three stay
 * in lock-step (see heap.h's own comment for the full rationale). This
 * out-of-line version remains the reference implementation and is
 * still called directly from op_select.S/op_select_or.S/op_with.S. */
Value ll_resolve_cell_chain(Value v) {
    int64_t i = 0;
    while (ll_value_is_cell(v)) {
        if (++i > LL_CELL_CHASE_LIMIT) {
            static const char msg[] = "ll: infinite recursion forcing a thunk (blackhole), aborting\n";
            fwrite(msg, 1, sizeof(msg) - 1, stderr);
            exit(1);
        }
        v = *ll_cell_target(v);
    }
    return v;
}

/* See heap.h's LL_LAZYAPP_MARKER comment for the full rationale/layout. */
Value ll_make_lazyapp1(Value fn, Value arg1) {
    void *mem = ll_arena_alloc(LL_LAZYAPP_SIZE);
    ll_stats_lazyapp++;
    *(int64_t *)((char *)mem + LL_LAZYAPP_OFF_MARKER) = LL_LAZYAPP_MARKER;
    *(Value *)((char *)mem + LL_LAZYAPP_OFF_FN) = fn;
    *(Value *)((char *)mem + LL_LAZYAPP_OFF_ARG1) = arg1;
    *(Value *)((char *)mem + LL_LAZYAPP_OFF_ARG2) = (Value){ .raw = 0 }; /* "no arg2" sentinel */
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | LL_TAG_PTR };
}

Value ll_make_lazyapp2(Value fn, Value arg1, Value arg2) {
    void *mem = ll_arena_alloc(LL_LAZYAPP_SIZE);
    ll_stats_lazyapp++;
    *(int64_t *)((char *)mem + LL_LAZYAPP_OFF_MARKER) = LL_LAZYAPP_MARKER;
    *(Value *)((char *)mem + LL_LAZYAPP_OFF_FN) = fn;
    *(Value *)((char *)mem + LL_LAZYAPP_OFF_ARG1) = arg1;
    *(Value *)((char *)mem + LL_LAZYAPP_OFF_ARG2) = arg2;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | LL_TAG_PTR };
}

int ll_value_is_lazyapp(Value v) {
    if (ll_tag_of(v) != LL_TAG_PTR) return 0;
    const char *base = (const char *)(v.raw & LL_PTR_BASE_MASK);
    int64_t marker = *(const int64_t *)(base + LL_LAZYAPP_OFF_MARKER);
    return marker == LL_LAZYAPP_MARKER || marker == LL_LAZYAPP_FORCED_MARKER;
}

Value ll_resolve_lazyapp(Value v) {
    char *base = (char *)(v.raw & LL_PTR_BASE_MASK);
    int64_t marker = *(int64_t *)(base + LL_LAZYAPP_OFF_MARKER);
    if (marker == LL_LAZYAPP_FORCED_MARKER) {
        return *(Value *)(base + LL_LAZYAPP_OFF_RESULT);
    }
    Value fn = *(Value *)(base + LL_LAZYAPP_OFF_FN);
    Value arg1 = *(Value *)(base + LL_LAZYAPP_OFF_ARG1);
    Value arg2 = *(Value *)(base + LL_LAZYAPP_OFF_ARG2);
    Value result = (arg2.raw == 0) ? ll_apply(fn, &arg1, 1) : ll_bshared_apply2(fn, arg1, arg2);
    /* Memoize in place -- see heap.h's comment for why this is safe to do
     * unconditionally here (only ever reached after `fn` has already
     * returned successfully; an exception unwinding past the two calls
     * above never reaches this line at all, so there is no partial state
     * to clean up). */
    *(int64_t *)(base + LL_LAZYAPP_OFF_MARKER) = LL_LAZYAPP_FORCED_MARKER;
    *(Value *)(base + LL_LAZYAPP_OFF_RESULT) = result;
    return result;
}

/* See heap.h's LL_THUNKMAP_MARKER comment for the full rationale/layout
 * -- uses the same LL_PTR_BASE_MASK defined near the top of this file. */
static void *thunkmap_desc_of(Value v) {
    return (void *)(uintptr_t)(v.raw & LL_PTR_BASE_MASK);
}

static int64_t thunkmap_idx_of(Value v) {
    return (int64_t)(v.raw >> 48);
}

void *ll_thunkmap_make_desc(Value f, const void *src_entries) {
    void *mem = ll_arena_alloc(LL_THUNKMAP_SIZE);
    ll_stats_thunkmap_desc++;
    *(int64_t *)((char *)mem + LL_THUNKMAP_OFF_MARKER) = LL_THUNKMAP_MARKER;
    *(Value *)((char *)mem + LL_THUNKMAP_OFF_F) = f;
    *(const void **)((char *)mem + LL_THUNKMAP_OFF_SRC_ENTRIES) = src_entries;
    *(Value *)((char *)mem + LL_THUNKMAP_OFF_OUT) = (Value){ .raw = 0 }; /* patched by
        ll_thunkmap_set_out once the caller has built `out` -- see value_util.h's comment */
    return mem;
}

Value ll_thunkmap_pack(void *desc, int64_t idx) {
    ll_stats_thunkmap_elem++;
    uint64_t p = (uint64_t)(uintptr_t)desc;
    return (Value){ .raw = p | ((uint64_t)idx << 48) | LL_TAG_PTR };
}

void ll_thunkmap_set_out(void *desc, Value out) {
    *(Value *)((char *)desc + LL_THUNKMAP_OFF_OUT) = out;
}

int ll_value_is_thunkmap(Value v) {
    if (ll_tag_of(v) != LL_TAG_PTR) return 0;
    const void *base = thunkmap_desc_of(v);
    return *(const int64_t *)((const char *)base + LL_THUNKMAP_OFF_MARKER) == LL_THUNKMAP_MARKER;
}

Value *ll_thunkmap_slot(Value v) {
    void *desc = thunkmap_desc_of(v);
    int64_t idx = thunkmap_idx_of(v);
    Value out = *(Value *)((char *)desc + LL_THUNKMAP_OFF_OUT);
    char *entries = (char *)(uintptr_t)(out.raw & ~0x7ULL) + LL_ATTRS_OFF_ENTRIES;
    return (Value *)(entries + idx * LL_ATTR_SIZE + LL_ATTR_OFF_VALUE);
}

Value ll_resolve_thunkmap(Value v) {
    Value *slot = ll_thunkmap_slot(v);
    if (slot->raw != v.raw) {
        /* Already forced by some earlier reference to this same element --
         * see heap.h's comment for the "bit-identical" memoization proof. */
        return *slot;
    }
    void *desc = thunkmap_desc_of(v);
    int64_t idx = thunkmap_idx_of(v);
    Value f = *(Value *)((char *)desc + LL_THUNKMAP_OFF_F);
    const char *src_entries = *(const char **)((char *)desc + LL_THUNKMAP_OFF_SRC_ENTRIES);
    const char *src_entry = src_entries + idx * LL_ATTR_SIZE;
    uint32_t sym = *(const uint32_t *)(src_entry + LL_ATTR_OFF_SYM);
    Value value = *(const Value *)(src_entry + LL_ATTR_OFF_VALUE);
    size_t len;
    const char *name = ll_sym_name(sym, &len);
    Value name_val = ll_make_string_value(name, len);
    Value result = ll_bshared_apply2(f, name_val, value);
    /* Memoize in place -- see heap.h's comment for why this is safe to do
     * unconditionally here (only ever reached after `f` has already
     * returned successfully; an exception unwinding past the call above
     * never reaches this line at all, so there is no partial state to
     * clean up -- same reasoning as ll_resolve_lazyapp above). */
    *slot = result;
    return result;
}

/* ll-only, no asnix equivalent: see heap.h's LL_FORMALSMAP_MARKER
 * comment for the full design. Mirrors thunkmap_desc_of/idx_of above --
 * same packing scheme, same LL_PTR_BASE_MASK. */
static void *formalsmap_desc_of(Value v) {
    return (void *)(uintptr_t)(v.raw & LL_PTR_BASE_MASK);
}

static int64_t formalsmap_idx_of(Value v) {
    return (int64_t)(v.raw >> 48);
}

void *ll_formalsmap_make_desc(Value arg, const int64_t *syms, void *env) {
    void *mem = ll_arena_alloc(LL_FORMALSMAP_SIZE);
    *(int64_t *)((char *)mem + LL_FORMALSMAP_OFF_MARKER) = LL_FORMALSMAP_MARKER;
    *(Value *)((char *)mem + LL_FORMALSMAP_OFF_ARG) = arg;
    *(const int64_t **)((char *)mem + LL_FORMALSMAP_OFF_SYMS) = syms;
    *(void **)((char *)mem + LL_FORMALSMAP_OFF_ENV) = env;
    return mem;
}

Value ll_formalsmap_pack(void *desc, int64_t idx) {
    uint64_t p = (uint64_t)(uintptr_t)desc;
    return (Value){ .raw = p | ((uint64_t)idx << 48) | LL_TAG_PTR };
}

int ll_value_is_formalsmap(Value v) {
    if (ll_tag_of(v) != LL_TAG_PTR) return 0;
    const void *base = formalsmap_desc_of(v);
    return *(const int64_t *)((const char *)base + LL_FORMALSMAP_OFF_MARKER) == LL_FORMALSMAP_MARKER;
}

Value *ll_formalsmap_slot(Value v) {
    void *desc = formalsmap_desc_of(v);
    int64_t idx = formalsmap_idx_of(v);
    Env *env = *(Env **)((char *)desc + LL_FORMALSMAP_OFF_ENV);
    return &env->values[idx];
}

Value ll_resolve_formalsmap(Value v) {
    Value *slot = ll_formalsmap_slot(v);
    if (slot->raw != v.raw) {
        /* Already forced by some earlier reference to this same formal --
         * same bit-identical memoization proof as ThunkMap's own. */
        return *slot;
    }
    void *desc = formalsmap_desc_of(v);
    int64_t idx = formalsmap_idx_of(v);
    Value arg = *(Value *)((char *)desc + LL_FORMALSMAP_OFF_ARG);
    const int64_t *syms = *(const int64_t **)((char *)desc + LL_FORMALSMAP_OFF_SYMS);
    uint32_t sym = (uint32_t)syms[idx];
    /* OP_CHECK_FORMALS already validated this exact sym is present on
     * `arg` before this frame's own body could ever run -- this mechanism
     * only ever covers a REQUIRED formal (no default), so there is no
     * missing-field case to handle here, unlike a general select. */
    int64_t found = ll_bshared_attrs_find(arg, sym);
    Value raw_value = ll_bshared_attrs_value(arg, found);
    /* Match OP_SELECT's own forcing exactly -- a plain required-formal
     * binding compiles to a bare `arg.field` select, which always forces
     * its result (op_select.c's own ll_op_select calls
     * ll_force_for_print on the selected entry before pushing it). */
    Value result = ll_force_for_print(raw_value);
    *slot = result;
    return result;
}

/* Shared by ll_add_values/ll_sub_values/ll_mul_values/
 * ll_div_values/the four ll_cmp_*_values functions below: real
 * Nix promotes an int operand to float whenever the OTHER operand is
 * already a float (verified against `nix-instantiate --eval` for every
 * one of the four arithmetic operators, and for the four ordering
 * comparisons' CompareValues -- see
 * value_util.h's comments on each). Returns 1 and writes `*out` if `v`
 * is numeric (int or float), 0 (leaving `*out` untouched) otherwise --
 * callers combine this with their own fatal message, since each call
 * site's wording differs slightly (matching existing convention). */
static int as_double(Value v, double *out) {
    if (ll_value_is_float(v)) {
        *out = ll_float_value(v);
        return 1;
    }
    if (ll_tag_of(v) == LL_TAG_INT) {
        *out = (double)ll_int_value(v);
        return 1;
    }
    return 0;
}

const char *ll_ptr_bytes(Value v, int64_t *out_len) {
    const char *base = (const char *)(v.raw & ~0x7ULL);
    if (ll_value_is_path(v)) {
        *out_len = *(const int64_t *)(base + LL_PATH_OFF_LEN);
        return base + LL_PATH_OFF_BYTES;
    }
    *out_len = *(const int64_t *)(base + LL_STRING_OFF_LEN);
    return base + LL_STRING_OFF_BYTES;
}

Value ll_coerce_to_string(Value v) {
    char buf[32];
    switch (ll_tag_of(v)) {
        case LL_TAG_INT: {
            int n = snprintf(buf, sizeof(buf), "%lld", (long long)ll_int_value(v));
            return ll_make_string_value(buf, (size_t)n);
        }
        case LL_TAG_IMM:
            switch (ll_imm_subtag(v)) {
                /* Matches real Nix exactly (verified with `nix-instantiate --eval
                 * -E 'builtins.toString true/false/null'` -- *not* "true"/"false":
                 * real Nix's `toString` on a bool yields "1" for true and "" for
                 * false, same empty string as null. A prior version of this
                 * function returned "true"/"false" here, which is wrong -- see
                 * docs/architecture.md's "Known bugs" list. */
                case LL_IMM_TRUE: return ll_make_string_value("1", 1);
                case LL_IMM_FALSE: return ll_make_string_value("", 0);
                case LL_IMM_NULL: return ll_make_string_value("", 0);
                default: break;
            }
            break;
        case LL_TAG_PTR: {
            /* A Float formats via the SAME rule real Nix's own
             * `coerceToString` uses for `toString`/coerceMore (verified
             * against `nix-instantiate --eval`: `toString 1.5` ==
             * "1.500000", `toString (0.0 - 1.0/0.0 ... )`-style nan/inf
             * cases print "nan"/"-nan"/"inf"/"-inf") -- `std::to_string`
             * on a double is glibc's `%f`: always 6 digits after the
             * decimal point for a finite value, and for non-finite
             * values `%f`'s nan/inf spelling (with sign) falls out for
             * free, no special-casing needed. NOT the same formatting
             * the top-level value printer uses (main.c's
             * print_value_inner, "%.6g"-style) -- two genuinely
             * different real-Nix formatting rules for two different call
             * sites, confirmed by directly comparing `nix-instantiate
             * --eval '1.5'` ("1.5") against `nix-instantiate --eval
             * 'toString 1.5'` ("1.500000"). */
            if (ll_value_is_float(v)) {
                char buf[512]; /* worst case: %f of the largest finite double is ~317 bytes */
                int n = snprintf(buf, sizeof(buf), "%f", ll_float_value(v));
                return ll_make_string_value(buf, (size_t)n);
            }
            if (!ll_value_is_path(v)) return v; /* already a string */
            int64_t len;
            const char *bytes = ll_ptr_bytes(v, &len);
            return ll_make_string_value(bytes, (size_t)len); /* path -> string, see value_util.h */
        }
        case LL_TAG_ATTRS: {
            /* An attrset with a `__toString` or `outPath` attribute
             * coerces to a string via that attribute -- real Nix's
             * own `coerceToString` checks exactly these two, in this
             * order, before giving up (e.g. a derivation attrset's
             * `outPath` is exactly how `"${someDerivation}"` and
             * `toString someDerivation` work) -- found needed by
             * numtide/nix-conformance's eval-okay-pathexists.nix
             * (`builtins.toPath { __toString = x: ...; }` and
             * `{ outPath = ...; }`), verified against `nix-instantiate
             * --eval`. `__toString` is called with the attrset itself as
             * its one argument, matching real Nix; its result (and a
             * plain `outPath`'s value) is coerced recursively, since
             * either could itself be a path or another such object.
             * `outPath`/`__toString`'s own result may already carry
             * attached string context (e.g. a derivation's own output
             * path, see derivation.c's ll_prim_derivation) -- forward
             * it unchanged, same pointer-identity preservation every
             * other "just return the inner coercion" case here already
             * gets for free (no NEW context is ever synthesized in this
             * function, only forwarded -- that matches real Nix, whose
             * plain `toString` never triggers copyToStore and so never
             * synthesizes an Opaque ref, but still forwards whatever
             * context an input Value already carries). */
            uint32_t sym_tostring = ll_intern("__toString", 10);
            int64_t idx = ll_bshared_attrs_find(v, sym_tostring);
            if (idx >= 0) {
                Value fn = ll_force_for_print(ll_bshared_attrs_value(v, idx));
                Value result = ll_apply(fn, &v, 1);
                return ll_coerce_to_string(ll_force_for_print(result));
            }
            uint32_t sym_outpath = ll_intern("outPath", 7);
            idx = ll_bshared_attrs_find(v, sym_outpath);
            if (idx >= 0) {
                return ll_coerce_to_string(ll_force_for_print(ll_bshared_attrs_value(v, idx)));
            }
            break;
        }
        case LL_TAG_LIST: {
            /* A list coerces to a string by recursively coercing each
             * element and joining with a single space -- real
             * Nix's own `coerceToString`'s `coerceMore` branch does the
             * same (`~/nix/src/libexpr/eval.cc`), found needed by
             * `<nix/fetchurl.nix>`'s own `urls = [ url ];` derivation
             * attribute (every derivation attribute value is coerced to
             * a string for the environment, see `derivation.c`).
             * Verified against real `nix-instantiate --eval`:
             * `builtins.toString [1 2 3]` == `"1 2 3"`, `builtins.toString
             * [ "a" "b" ]` == `"a b"`, `builtins.toString []` == `""`.
             * Does not replicate cppnix's own "skip the separator before
             * an empty nested list" quirk (that function's own comment
             * calls it "not quite correct") -- not hit by anything this
             * project has evaluated so far.
             *
             * Each element's own context (if any) unions into the
             * joined result's context -- same pattern as
             * `concat_strings_impl`'s propagation. */
            int64_t count = ll_bshared_list_count(v);
            const Value *elems = ll_bshared_list_elems(v);
            char *result = NULL;
            size_t result_len = 0;
            LlStringContext ctx;
            ll_ctx_init(&ctx);
            for (int64_t i = 0; i < count; i++) {
                Value piece = ll_coerce_to_string(ll_force_for_print(elems[i]));
                ll_ctx_union_into(&ctx, ll_string_context_of(piece));
                int64_t piece_len;
                const char *piece_bytes = ll_ptr_bytes(piece, &piece_len);
                int has_sep = (i + 1 < count);
                result = realloc(result, result_len + (size_t)piece_len + (has_sep ? 1 : 0));
                memcpy(result + result_len, piece_bytes, (size_t)piece_len);
                result_len += (size_t)piece_len;
                if (has_sep) result[result_len++] = ' ';
            }
            Value out = ll_make_string_value(result ? result : "", result_len);
            free(result);
            ll_string_attach_context(out, &ctx);
            return out;
        }
        default: break;
    }
    fprintf(stderr, "ll: cannot coerce this value to a string\n");
    exit(1);
}

/* `derivation {...}`'s own attribute coercion (derivation.c) --
 * real Nix's `derivationStrictInternal` (`~/nix/src/libexpr/
 * primops.cc`) calls `coerceToString` with `coerceMore=true` (accept
 * int/bool/null, same permissiveness as `builtins.toString`) AND
 * `copyToStore=true` (a Path NOT already under `/nix/store/` gets
 * NAR-hashed and copied, same as genuine `"${...}"` string
 * interpolation, `coerce_to_string_for_interp` above) -- a
 * combination NEITHER existing coercion function provides on its own
 * (`ll_coerce_to_string` has the former but not the latter;
 * `coerce_to_string_for_interp` has the latter but rejects int/bool/
 * null outright). Confirmed needed directly: nixpkgs's own
 * `hello.drv` embeds `pkgs/stdenv/generic/source-stdenv.sh`/
 * `default-builder.sh` (plain local Path literals, in `args`) as
 * real `/nix/store/...` paths. Recurses into itself (not
 * `ll_coerce_to_string`) for the ATTRS `__toString`/`outPath` and
 * LIST cases, so a Path nested inside either also gets copied. */
Value ll_coerce_to_string_for_derivation(Value v) {
    if (ll_tag_of(v) == LL_TAG_PTR && ll_value_is_path(v)) {
        int64_t len;
        const char *bytes = ll_ptr_bytes(v, &len);
        char *nul_terminated = malloc((size_t)len + 1);
        memcpy(nul_terminated, bytes, (size_t)len);
        nul_terminated[len] = '\0';
        /* A bare Path, once coerced into a derivation attribute's
         * text, is exactly real Nix's `copyToStore=true` origin point
         * for an Opaque context entry -- whether it was ALREADY under
         * `/nix/store/` (a prior derivation's own output, reached as a
         * bare Path rather than through that derivation's attrset) or
         * just got NAR-copied there below. This replaces
         * `ll_collect_input_refs`'s structural bare-Path detection
         * entirely: that walk is blind to a Path reached only AFTER a
         * prior string interpolation, this is not. */
        if (len > 11 && memcmp(nul_terminated, "/nix/store/", 11) == 0) {
            Value out = ll_make_string_value(nul_terminated, (size_t)len);
            LlStringContext ctx;
            ll_ctx_init(&ctx);
            ll_ctx_add_opaque(&ctx, nul_terminated);
            ll_string_attach_context(out, &ctx);
            free(nul_terminated);
            return out;
        }
        /* Perf follow-up (strace-driven, nar.h's own comment): skip the
         * existence-check `lstat` entirely when this exact literal path
         * was already successfully copied earlier in this process -- the
         * `ll_copy_path_to_store` call below would just hit its own
         * cache anyway, so the `lstat` above would be pure syscall
         * overhead repeated on every one of this path's (frequently
         * thousands of) call sites. */
        const char *already = ll_copy_path_to_store_cache_lookup(nul_terminated);
        char store_path[300];
        if (already) {
            snprintf(store_path, sizeof(store_path), "%s", already);
        } else {
            struct stat st;
            if (lstat(nul_terminated, &st) != 0) {
                fprintf(stderr, "ll: path '%s' does not exist\n", nul_terminated);
                free(nul_terminated);
                exit(1);
            }
            ll_copy_path_to_store(nul_terminated, store_path, sizeof(store_path));
        }
        free(nul_terminated);
        Value out = ll_make_string_value(store_path, strlen(store_path));
        LlStringContext ctx;
        ll_ctx_init(&ctx);
        ll_ctx_add_opaque(&ctx, store_path);
        ll_string_attach_context(out, &ctx);
        return out;
    }
    if (ll_tag_of(v) == LL_TAG_ATTRS) {
        uint32_t sym_tostring = ll_intern("__toString", 10);
        int64_t idx = ll_bshared_attrs_find(v, sym_tostring);
        if (idx >= 0) {
            Value fn = ll_force_for_print(ll_bshared_attrs_value(v, idx));
            Value result = ll_apply(fn, &v, 1);
            return ll_coerce_to_string_for_derivation(ll_force_for_print(result));
        }
        uint32_t sym_outpath = ll_intern("outPath", 7);
        idx = ll_bshared_attrs_find(v, sym_outpath);
        if (idx >= 0) {
            return ll_coerce_to_string_for_derivation(ll_force_for_print(ll_bshared_attrs_value(v, idx)));
        }
    }
    if (ll_tag_of(v) == LL_TAG_LIST) {
        int64_t count = ll_bshared_list_count(v);
        const Value *elems = ll_bshared_list_elems(v);
        char *result = NULL;
        size_t result_len = 0;
        LlStringContext ctx;
        ll_ctx_init(&ctx);
        for (int64_t i = 0; i < count; i++) {
            Value piece = ll_coerce_to_string_for_derivation(ll_force_for_print(elems[i]));
            ll_ctx_union_into(&ctx, ll_string_context_of(piece));
            int64_t piece_len;
            const char *piece_bytes = ll_ptr_bytes(piece, &piece_len);
            int has_sep = (i + 1 < count);
            result = realloc(result, result_len + (size_t)piece_len + (has_sep ? 1 : 0));
            memcpy(result + result_len, piece_bytes, (size_t)piece_len);
            result_len += (size_t)piece_len;
            if (has_sep) result[result_len++] = ' ';
        }
        Value out = ll_make_string_value(result ? result : "", result_len);
        free(result);
        ll_string_attach_context(out, &ctx);
        return out;
    }
    return ll_coerce_to_string(v);
}

/* String *interpolation* coercion is strictly narrower than `toString`'s
 * (ll_coerce_to_string, above): `"${7}"`/`"${true}"`/`"${null}"` all fail
 * with "cannot coerce ... to a string", even though `builtins.toString
 * 7`/`true`/`null` succeed. Real Nix's own EvalState::coerceToString has
 * a `coerceMore` flag; `toString` passes true, plain "${...}"
 * interpolation passes false -- only strings, paths, and sets with a
 * `__toString`/`outPath` attr are accepted without it. */
/* `check_path_exists` -- true for genuine `"${...}"` string
 * interpolation (OP_STR_CONCAT, via `ll_concat_strings` below), false
 * for the UNRELATED internal reuse of this same coercion logic by the
 * `+` operator's path-concat case (`ll_add_values`, via
 * `concat_strings_impl` below). Real Nix's own path-existence
 * requirement is specific to interpolation (string coercion that would
 * trigger a copy-to-store) -- `+` on a Path is ordinary text
 * concatenation with no such requirement (confirmed against real
 * `nix-instantiate --eval`: `toString (/foo/bar + "/x")` succeeds even
 * when `/foo/bar` doesn't exist, while `"${./bar}"` on the same
 * nonexistent path fails) -- found as a regression this fix introduced
 * (`tests/e2e/m57g_path_canon_concat.nix`, path `+` string, broke when
 * the existence check below was first added unconditionally).
 *
 * Follow-up (chasing d.nix/nixpkgs-hash-parity): `copy_to_store` is a
 * SEPARATE flag from `check_path_exists`, not implied by it. Real Nix's
 * `ExprConcatStrings::eval` (~/nix/src/libexpr/eval.cc) calls
 * `coerceToString` with `coerceMore=false`, `copyToStore=(firstType ==
 * nString)`, `canonicalizePath=!first` for EVERY operand of a
 * `+`/string-concat chain -- `copyToStore` tracks
 * whether the chain's FIRST operand was a plain string (never gated on
 * existence-checking at all). A `+` chain whose first operand is a
 * string (e.g. nixpkgs's own `minimal-bootstrap/mes/libc.nix`:
 * `libc_gnu1_SOURCES + " " + ./ldexpl.c`) DOES copy a later Path operand
 * to the store and attach its Opaque context -- confirmed directly
 * against a real `.drv`: the resulting env var's text embeds
 * `/nix/store/<hash>-ldexpl.c`, and that path is listed in `inputSrcs`,
 * not the bare local filesystem path. `ll_add_values` below passes
 * `copy_to_store = !ll_value_is_path(lhs)` -- valid because asnix's
 * binary-chain structure always re-wraps an intermediate `+` result as a
 * Path (via `canon_wrap_as_path`) exactly when the chain's first operand
 * was a Path, so checking the immediate `lhs`'s tag is equivalent to
 * real Nix's single `firstType` tracked across the whole n-ary chain. */
static Value coerce_to_string_for_interp(Value v, int check_path_exists, int copy_to_store) {
    if (ll_tag_of(v) == LL_TAG_PTR) {
        /* A Path is a second, differently-shaped LL_TAG_PTR kind (see
         * value_util.h) -- returning it unchanged, as if every TAG_PTR were
         * already a plain String, reads its marker field as a bogus string
         * length and corrupts memory (found interpolating a path literal:
         * `"${./foo}"` segfaulted). Real Nix accepts a path here without
         * `builtins.toString` (unlike ints/bools/null -- see this
         * function's own comment above), so reject isn't right either;
         * coerce it the same way `ll_coerce_to_string` does. */
        if (ll_value_is_path(v)) {
            /* Real Nix additionally requires the path *exist* on
             * disk at this specific call site -- string interpolation of
             * a bare path triggers "copy to store" coercion (unlike
             * `builtins.toString`, which just formats the text and never
             * checks existence: confirmed `toString ./nonexistent`
             * succeeds against real `nix-instantiate --eval`, while
             * `"${./nonexistent}"` fails with `error: path '...' does
             * not exist`). `lstat`, not `stat`, so a dangling symlink
             * still counts as "exists" -- matches `builtins.pathExists`'s
             * own existing convention in builtins_m23.c. Gated on
             * `check_path_exists` -- see this function's own header
             * comment for why `+`'s reuse of this same coercion needs to
             * skip it (that case is text reassembly of a NEW path
             * literal, not a Path VALUE being embedded in a string). */
            int64_t len;
            const char *bytes = ll_ptr_bytes(v, &len);
            char *nul_terminated = malloc((size_t)len + 1);
            memcpy(nul_terminated, bytes, (size_t)len);
            nul_terminated[len] = '\0';
            /* Perf follow-up (strace-driven, nar.h's own comment): when
             * this call is also going to copy-to-store (below) and this
             * exact literal path was already copied earlier in this
             * process, skip the existence-check `lstat` -- the copy
             * below will just hit its own cache, so a successful earlier
             * copy already proves the path exists; re-`lstat`-ing it on
             * every one of its (frequently thousands of) call sites is
             * pure syscall overhead. */
            if (check_path_exists && !(copy_to_store && ll_copy_path_to_store_cache_lookup(nul_terminated))) {
                struct stat st;
                if (lstat(nul_terminated, &st) != 0) {
                    fprintf(stderr, "ll: path '%s' does not exist\n", nul_terminated);
                    free(nul_terminated);
                    exit(1);
                }
            }
            /* A path already under `/nix/store/` (e.g. a prior
             * derivation's own output) is used verbatim -- real Nix
             * never re-copies something that's already in the store.
             * Anything else is NAR-hashed and copied (see
             * ll_copy_path_to_store), gated on `copy_to_store` -- see
             * this function's own header comment (follow-up): this flag
             * is real Nix's `copyToStore` exactly, independent of
             * `check_path_exists`. */
            /* Same Opaque-context origin point as
             * ll_coerce_to_string_for_derivation's identical Path
             * branch -- see its own comment. Only reached when
             * `copy_to_store`, matching exactly when the store-copy
             * itself is gated. */
            if (copy_to_store) {
                int in_store = len > 11 && memcmp(nul_terminated, "/nix/store/", 11) == 0;
                char store_path[300];
                const char *final_path = nul_terminated;
                if (!in_store) {
                    ll_copy_path_to_store(nul_terminated, store_path, sizeof(store_path));
                    final_path = store_path;
                }
                Value out = ll_make_string_value(final_path, strlen(final_path));
                LlStringContext ctx;
                ll_ctx_init(&ctx);
                ll_ctx_add_opaque(&ctx, final_path);
                ll_string_attach_context(out, &ctx);
                free(nul_terminated);
                return out;
            }
            free(nul_terminated);
            return ll_coerce_to_string(v);
        }
        /* A bare Float is a THIRD such kind -- same memory-corruption
         * risk as Path if returned unchanged (its marker+double bytes
         * would be misread as a string length+content). Unlike Path, a
         * Float is REJECTED here, same as an int (verified against
         * `nix-instantiate --eval`: `"${1.5}"` fails "cannot coerce a
         * float to a string", while `"${toString 1.5}"` succeeds --
         * `coerceToString`'s `coerceMore` branch, which handles nFloat,
         * is exactly the branch `toString` takes and bare interpolation
         * does not, same split this function's own header comment
         * already describes for int/bool/null). Falls through to the
         * generic rejection below, which already knows how to name a
         * Float via ll_tag_of(v)==LL_TAG_PTR's... no -- that
         * switch is INT-only for "an integer"; handled explicitly right
         * here instead since the switch below has no float case of its
         * own. */
        if (ll_value_is_float(v)) {
            fprintf(stderr, "ll: cannot coerce a float to a string in a string interpolation "
                             "(use builtins.toString explicitly)\n");
            exit(1);
        }
        return v;
    }
    if (ll_tag_of(v) == LL_TAG_ATTRS) {
        /* An attrset with `__toString`/`outPath` IS accepted here even
         * though int/bool/null/list/plain-attrset aren't --
         * real Nix's own `coerceMore=false` path still special-cases
         * exactly this (verified: `"${derivation {...}}"` interpolates
         * fine via its `outPath`, while `"${{}}""` -- a plain attrset
         * with neither attribute -- still fails the same as any other
         * disallowed type). Delegate to ll_coerce_to_string, which
         * already implements the __toString/outPath lookup -- if
         * neither is present it fatals with its own (slightly
         * differently worded, but equally clear) "cannot coerce"
         * message, which is fine since that's the "a plain set" case
         * real Nix also rejects here regardless of wording. */
        uint32_t sym_tostring = ll_intern("__toString", 10);
        uint32_t sym_outpath = ll_intern("outPath", 7);
        if (ll_bshared_attrs_find(v, sym_tostring) >= 0 || ll_bshared_attrs_find(v, sym_outpath) >= 0) {
            return ll_coerce_to_string(v);
        }
    }
    const char *kind = "this value";
    switch (ll_tag_of(v)) {
        case LL_TAG_INT: kind = "an integer"; break;
        case LL_TAG_IMM:
            switch (ll_imm_subtag(v)) {
                case LL_IMM_TRUE:
                case LL_IMM_FALSE: kind = "a boolean"; break;
                case LL_IMM_NULL: kind = "null"; break;
                default: break;
            }
            break;
        case LL_TAG_LIST: kind = "a list"; break;
        case LL_TAG_ATTRS: kind = "a set"; break;
        case LL_TAG_CLOSURE:
        case LL_TAG_PRIMOP: kind = "a function"; break;
        default: break;
    }
    fprintf(stderr, "ll: cannot coerce %s to a string in a string interpolation "
                     "(use builtins.toString explicitly)\n", kind);
    exit(1);
}

/* `builtins.concatStringsSep`'s own per-element coercion
 * (builtins_misc.c's concatStringsSep2_impl) -- real Nix's
 * `prim_concatStringsSep` (`~/nix/src/libexpr/primops.cc`) calls
 * `EvalState::coerceToString` with its default arguments, i.e.
 * `coerceMore=false, copyToStore=true` -- the EXACT same coercion rules
 * as genuine `"${...}"` string interpolation (strings/paths/attrsets
 * with `__toString`/`outPath` accepted; ints/bools/null/lists rejected,
 * since those need `coerceMore=true`, which only `builtins.toString`
 * passes), not `toString`'s more permissive rules. Confirmed needed by
 * nixpkgs's curl build: some `concatStringsSep`-driven derivation
 * attribute's list genuinely contains a derivation attrset (e.g. a
 * `buildInputs`/`propagatedBuildInputs` entry threaded through
 * unmodified rather than pre-stringified via `"${x}/bin"`-style
 * interpolation before reaching the list) -- `concatStringsSep2_impl`
 * previously required every element to already be a bare LL_TAG_PTR
 * string (via its own stricter `as_string` helper), which fatals on
 * that attrset even though real Nix accepts it via exactly this
 * coercion (peeling its `outPath`). Just a thin exported wrapper around
 * `coerce_to_string_for_interp` with `check_path_exists=1,
 * copy_to_store=1` -- same as `ll_concat_strings`'s own use of it
 * below -- so a bare Path element (if one ever appears here) is copied
 * to the store the same way a `"${somePath}"` interpolation's would
 * be, not given special-cased, narrower treatment. */
Value ll_coerce_to_string_for_concat_sep(Value v) {
    return coerce_to_string_for_interp(v, 1, 1);
}

/* Shared by `ll_concat_strings` (real `"${...}"` interpolation,
 * `check_path_exists=1, copy_to_store=1`) and `ll_add_values`'s
 * path-concat case (`+`, `check_path_exists=0` always, `copy_to_store`
 * depending on the chain's first operand) -- see
 * `coerce_to_string_for_interp`'s own header comment for why the two
 * flags are independent despite sharing every other coercion rule. */
static Value concat_strings_impl(const Value *parts, int64_t count, int check_path_exists, int copy_to_store) {
    if (count <= 0) return ll_make_string_value("", 0);

    Value *coerced = malloc((size_t)count * sizeof(Value));
    int64_t total_len = 0;
    LlStringContext ctx;
    ll_ctx_init(&ctx);
    for (int64_t i = 0; i < count; i++) {
        coerced[i] = coerce_to_string_for_interp(parts[i], check_path_exists, copy_to_store);
        ll_ctx_union_into(&ctx, ll_string_context_of(coerced[i]));
        const char *s = (const char *)(coerced[i].raw & ~0x7ULL);
        total_len += *(const int64_t *)(s + LL_STRING_OFF_LEN);
    }

    void *mem = ll_arena_alloc(8 + (size_t)total_len);
    *(int64_t *)((char *)mem + LL_STRING_OFF_LEN) = total_len;
    char *out = (char *)mem + LL_STRING_OFF_BYTES;
    for (int64_t i = 0; i < count; i++) {
        const char *s = (const char *)(coerced[i].raw & ~0x7ULL);
        int64_t len = *(const int64_t *)(s + LL_STRING_OFF_LEN);
        memcpy(out, s + LL_STRING_OFF_BYTES, (size_t)len);
        out += len;
    }
    free(coerced);
    Value result = (Value){ .raw = (uint64_t)(uintptr_t)mem | LL_TAG_PTR };
    /* `ll_concat_path` (path-literal interpolation/`~`)
     * also calls this but then rewraps the result as a PATH, not a
     * string -- Paths never carry context in real Nix (only strings
     * do), so attaching here and letting canon_wrap_as_path's own
     * fresh Path value simply not have an entry is correct; no special
     * casing needed for that caller. */
    ll_string_attach_context(result, &ctx);
    return result;
}

/* Shared by ll_add_values (`path + string`) and ll_concat_path
 * (`~`/path-literal interpolation): re-canonicalizes a plain String
 * Value's bytes (which must already start with '/' -- true for both
 * callers, since each only reaches this once it's established the
 * concatenation's first operand was already an absolute Path) and
 * rewraps it as a Path. */
static Value canon_wrap_as_path(Value string_result) {
    int64_t len;
    const char *bytes = ll_ptr_bytes(string_result, &len);
    char *nul_terminated = malloc((size_t)len + 1);
    memcpy(nul_terminated, bytes, (size_t)len);
    nul_terminated[len] = '\0';
    char *canon = ll_canon_path(nul_terminated);
    free(nul_terminated);
    Value out = ll_make_path_value(canon, strlen(canon));
    free(canon);
    return out;
}

/* The runtime half of path-literal interpolation (`./${foo}`,
 * `~/${foo}`, `/${foo}`, ...) -- see OP_PATH_CONCAT's comment in ir.h for
 * why unconditionally canon-wrapping is safe here (compiler.c's AST_PATH
 * case only ever emits this op when `parts[0]` is already a Path
 * constant). Calls `concat_strings_impl` directly with
 * `check_path_exists=0` (NOT the public `ll_concat_strings` wrapper,
 * which defaults to `check_path_exists=1`) -- a path LITERAL's
 * interpolated parts are plain text being assembled into a new path, not
 * a Path VALUE being embedded in a string, so the existence check
 * (which exists specifically for the latter, `"${somePath}"`) must not
 * apply here; same distinction `ll_add_values`'s own `path + string`
 * case (right below) already makes for the identical reason. Re-
 * canonicalizes the WHOLE result after concatenation, matching real
 * Nix's `ExprConcatStrings::eval` exactly (it re-canonicalizes
 * unconditionally too, even though the first chunk's own text was
 * deliberately left un-canonicalized when it was built -- see
 * resolve_path_chunk in compiler.c). */
Value ll_concat_path(const Value *parts, int64_t count) {
    /* firstType == nPath for this chain (parts[0] is always a Path
     * constant, per this function's own header comment) -- matches real
     * Nix's `copyToStore = firstType == nString` being false here. */
    Value result = concat_strings_impl(parts, count, 0, 0);
    return canon_wrap_as_path(result);
}

Value ll_concat_strings(const Value *parts, int64_t count) {
    return concat_strings_impl(parts, count, 1, 1);
}

Value ll_add_values(Value lhs, Value rhs) {
    int lt = ll_tag_of(lhs), rt = ll_tag_of(rhs);
    if (lt == LL_TAG_INT && rt == LL_TAG_INT) {
        return ll_make_int(ll_int_value(lhs) + ll_int_value(rhs));
    }
    /* int/float or float/float -- promote and add as doubles, same
     * rule as real Nix's `+` (ExprConcatStrings::eval, ~/nix's eval.cc:
     * "Upgrade the type from int to float" once either operand is a
     * float). Checked BEFORE the plain-int-vs-non-int fatal below and
     * the string/path concat fallback after it, since a float is
     * neither. */
    if (ll_value_is_float(lhs) || ll_value_is_float(rhs)) {
        double a, b;
        if (!as_double(lhs, &a)) {
            fprintf(stderr, "ll: '+': cannot add a non-numeric value to a float\n");
            exit(1);
        }
        if (!as_double(rhs, &b)) {
            fprintf(stderr, "ll: '+': cannot add a non-numeric value to a float\n");
            exit(1);
        }
        return ll_make_float_value(a + b);
    }
    if (lt == LL_TAG_INT || rt == LL_TAG_INT) {
        fprintf(stderr, "ll: '+': cannot add an integer and a non-integer\n");
        exit(1);
    }
    Value parts[2] = { lhs, rhs };
    /* `+` never requires a Path to exist (check_path_exists=0).
     * Follow-up (chasing d.nix/nixpkgs-hash-parity): `copy_to_store`,
     * unlike `check_path_exists`, is NOT unconditionally 0 here -- real
     * Nix's `copyToStore = firstType == nString` (~/nix/src/libexpr/
     * eval.cc's `ExprConcatStrings::eval`) ties it to whether the WHOLE
     * `+` chain's first operand was a string, not a path. `lhs` (this
     * call's left operand) is exactly that: asnix's binary-chain
     * `+`-folding always re-wraps an intermediate result as a Path (via
     * `canon_wrap_as_path` below) whenever the chain started with a
     * Path, so `lhs` already being a non-Path here means the chain's
     * first operand was a string -- confirmed needed directly:
     * nixpkgs's own `libc_gnu1_SOURCES + " " + ./ldexpl.c`
     * (minimal-bootstrap/mes/libc.nix) is a `+` chain starting with a
     * string, and real Nix's resulting `.drv` embeds `./ldexpl.c` as a
     * real copied `/nix/store/...` path (also listed in `inputSrcs`),
     * not the bare local filesystem path this project previously left
     * unconverted (copy_to_store was always 0 for `+` before this). */
    Value result = concat_strings_impl(parts, 2, 0, !ll_value_is_path(lhs));
    if (ll_value_is_path(lhs)) {
        /* Canonicalize `.`/`..`/doubled-slash segments the result
         * may now have (e.g. `/foo/bar + "/../xyzzy/./foo.txt"` ==
         * `/foo/xyzzy/foo.txt`), same as a path *literal* already gets
         * at compile time (compiler.c's AST_PATH case) -- a runtime
         * `path + string` concatenation previously just kept the raw,
         * uncollapsed text. `result`'s bytes always start with '/'
         * here since `lhs` is already an absolute Path. Shares
         * canon_wrap_as_path with ll_concat_path now, same logic. */
        return canon_wrap_as_path(result);
    }
    return result;
}

/* `-`/`*`'s slow-path fallback -- see value_util.h's comment. Only
 * ever called when at least one operand isn't a plain int (the all-int
 * case is already handled by op_arith.S's own fast path before this is
 * reached), so getting here with two non-floats is already a type
 * error. */
Value ll_sub_values(Value lhs, Value rhs) {
    double a, b;
    if (as_double(lhs, &a) && as_double(rhs, &b)) return ll_make_float_value(a - b);
    fprintf(stderr, "ll: '-': operands must both be numbers (int or float), aborting\n");
    exit(1);
}

Value ll_mul_values(Value lhs, Value rhs) {
    double a, b;
    if (as_double(lhs, &a) && as_double(rhs, &b)) return ll_make_float_value(a * b);
    fprintf(stderr, "ll: '*': operands must both be numbers (int or float), aborting\n");
    exit(1);
}

Value ll_div_values(Value lhs, Value rhs) {
    double a, b;
    if (!as_double(lhs, &a) || !as_double(rhs, &b)) {
        fprintf(stderr, "ll: '/': operands must both be numbers (int or float), aborting\n");
        exit(1);
    }
    /* Matches real Nix exactly: the float-coerced divisor is checked
     * against zero unconditionally, even when both operands started out
     * as ints and would otherwise divide evenly as floats (e.g. there is
     * no IEEE-754 "divide by zero gives infinity" case here at all --
     * see value_util.h's comment). The pure-int/int zero check already
     * lives in op_arith.S's own fast path; this is the one case that
     * reaches here with a zero divisor: an int/float or float/float pair. */
    if (b == 0.0) {
        fprintf(stderr, "ll: division by zero, aborting\n");
        exit(1);
    }
    return ll_make_float_value(a / b);
}

/* True iff `v` is a genuine String -- LL_TAG_PTR but neither a Path
 * nor a Float, the two other kinds that share the tag (see heap.h).
 * Shared by the four ordering fallbacks below for their string<string
 * case (same scope real Nix's `<`/`>`/`<=`/`>=` support beyond numbers:
 * `~/nix/src/libexpr/primops.cc`'s `CompareValues` also orders paths and
 * lists, but this project's existing `ll_bshared_value_lt` -- used by
 * `sort`/`lessThan` -- already deliberately narrows that to "INT<INT or
 * string<string only", and this mirrors that same already-accepted
 * scope cut for the operators rather than inventing a wider one). */
static int is_plain_string(Value v) {
    return ll_tag_of(v) == LL_TAG_PTR && !ll_value_is_path(v) && !ll_value_is_float(v);
}

/* Lexicographic byte comparison, shorter-is-less on a shared prefix --
 * same rule as ll_bshared_value_lt's existing string<string case
 * (builtins_shared.c), reimplemented here rather than shared across the
 * two files for the same reason value_util.c and builtins_shared.c
 * already don't share this kind of tiny helper elsewhere (no import
 * cycle: builtins_shared.c depends on value_util.h, not the reverse). */
static int str_lt(Value a, Value b) {
    int64_t la, lb;
    const char *ba = ll_ptr_bytes(a, &la);
    const char *bb = ll_ptr_bytes(b, &lb);
    int64_t n = la < lb ? la : lb;
    int c = n > 0 ? memcmp(ba, bb, (size_t)n) : 0;
    if (c != 0) return c < 0;
    return la < lb;
}

Value ll_cmp_lt_values(Value lhs, Value rhs) {
    double a, b;
    if (as_double(lhs, &a) && as_double(rhs, &b)) return a < b ? LL_TRUE : LL_FALSE;
    if (is_plain_string(lhs) && is_plain_string(rhs)) return str_lt(lhs, rhs) ? LL_TRUE : LL_FALSE;
    fprintf(stderr, "ll: comparison (< > <= >=) operands must both be numbers or both be strings, aborting\n");
    exit(1);
}

Value ll_cmp_gt_values(Value lhs, Value rhs) {
    double a, b;
    if (as_double(lhs, &a) && as_double(rhs, &b)) return a > b ? LL_TRUE : LL_FALSE;
    if (is_plain_string(lhs) && is_plain_string(rhs)) return str_lt(rhs, lhs) ? LL_TRUE : LL_FALSE;
    fprintf(stderr, "ll: comparison (< > <= >=) operands must both be numbers or both be strings, aborting\n");
    exit(1);
}

Value ll_cmp_le_values(Value lhs, Value rhs) {
    double a, b;
    if (as_double(lhs, &a) && as_double(rhs, &b)) return a <= b ? LL_TRUE : LL_FALSE;
    if (is_plain_string(lhs) && is_plain_string(rhs)) return !str_lt(rhs, lhs) ? LL_TRUE : LL_FALSE;
    fprintf(stderr, "ll: comparison (< > <= >=) operands must both be numbers or both be strings, aborting\n");
    exit(1);
}

Value ll_cmp_ge_values(Value lhs, Value rhs) {
    double a, b;
    if (as_double(lhs, &a) && as_double(rhs, &b)) return a >= b ? LL_TRUE : LL_FALSE;
    if (is_plain_string(lhs) && is_plain_string(rhs)) return !str_lt(lhs, rhs) ? LL_TRUE : LL_FALSE;
    fprintf(stderr, "ll: comparison (< > <= >=) operands must both be numbers or both be strings, aborting\n");
    exit(1);
}

Value ll_concat_lists(Value lhs, Value rhs) {
    if (ll_tag_of(lhs) != LL_TAG_LIST) {
        fprintf(stderr, "ll: expected a list but found a non-list value (lhs of '++')\n");
        exit(1);
    }
    if (ll_tag_of(rhs) != LL_TAG_LIST) {
        fprintf(stderr, "ll: expected a list but found a non-list value (rhs of '++')\n");
        exit(1);
    }
    const char *lbase = (const char *)(uintptr_t)(lhs.raw & ~(uint64_t)LL_TAG_MASK);
    const char *rbase = (const char *)(uintptr_t)(rhs.raw & ~(uint64_t)LL_TAG_MASK);
    int64_t lcount = *(const int64_t *)(lbase + LL_LIST_OFF_COUNT);
    int64_t rcount = *(const int64_t *)(rbase + LL_LIST_OFF_COUNT);
    int64_t total = lcount + rcount;

    void *mem = ll_arena_alloc(8 + (uint64_t)total * sizeof(Value));
    *(int64_t *)((char *)mem + LL_LIST_OFF_COUNT) = total;
    Value *out = (Value *)((char *)mem + LL_LIST_OFF_ELEMS);
    if (lcount > 0) {
        memcpy(out, lbase + LL_LIST_OFF_ELEMS, (size_t)lcount * sizeof(Value));
    }
    if (rcount > 0) {
        memcpy(out + lcount, rbase + LL_LIST_OFF_ELEMS, (size_t)rcount * sizeof(Value));
    }
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | LL_TAG_LIST };
}

/* Moved here from compiler.c so ll_add_values can reuse it for
 * `path + string` concatenation -- see this function's own long comment
 * in value_util.h for why, and compiler.c's AST_PATH case for the
 * original, still-current, compile-time-literal use. */
char *ll_canon_path(const char *in) {
    size_t len = strlen(in);
    char *out = malloc(len + 2);
    char **comps = malloc((len + 1) * sizeof(char *));
    size_t ncomps = 0;

    size_t i = 0;
    while (i < len) {
        while (i < len && in[i] == '/') i++;
        size_t start = i;
        while (i < len && in[i] != '/') i++;
        size_t clen = i - start;
        if (clen == 0) continue;
        if (clen == 1 && in[start] == '.') continue; /* "." -- drop */
        if (clen == 2 && in[start] == '.' && in[start + 1] == '.') {
            if (ncomps > 0) ncomps--; /* ".." -- pop (no-op at root, like real Nix) */
            continue;
        }
        char *c = malloc(clen + 1);
        memcpy(c, in + start, clen);
        c[clen] = '\0';
        comps[ncomps++] = c;
    }

    size_t pos = 0;
    for (size_t k = 0; k < ncomps; k++) {
        out[pos++] = '/';
        size_t clen = strlen(comps[k]);
        memcpy(out + pos, comps[k], clen);
        pos += clen;
        free(comps[k]);
    }
    free(comps);
    if (pos == 0) out[pos++] = '/';
    out[pos] = '\0';
    return out;
}

char *ll_string_value_to_cstr(Value string_val) {
    const char *s = (const char *)(string_val.raw & ~0x7ULL);
    int64_t len = *(const int64_t *)(s + LL_STRING_OFF_LEN);
    char *out = ll_arena_alloc((size_t)len + 1);
    memcpy(out, s + LL_STRING_OFF_BYTES, (size_t)len);
    out[len] = '\0';
    return out;
}
