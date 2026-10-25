/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "builtins_misc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/irbuild.h"
#include "base32.h"
#include "builtins_shared.h"
#include "hash_shim.h"
#include "store_path.h"
#include "string_context.h" /* M76 */
#include "value_util.h"

/* ---------------------------------------------------------------------
 * Small shared helpers (deliberately re-declared here rather than shared
 * with builtins.c's copies -- this file exists specifically so it has no
 * textual overlap with builtins.c for sibling agents working in
 * parallel; a later integration pass can dedupe if it wants to).
 * --------------------------------------------------------------------- */

static Value make_primop_value(void *fn, int64_t arity) {
    void *mem = asnix_arena_alloc(ASNIX_PRIMOP_SIZE);
    *(void **)((char *)mem + ASNIX_PRIMOP_OFF_FN) = fn;
    *(int64_t *)((char *)mem + ASNIX_PRIMOP_OFF_ARITY) = arity;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_PRIMOP };
}

static void fatal(const char *msg) {
    fprintf(stderr, "asnix: %s\n", msg);
    exit(1);
}

/* Reads a string Value's {len, bytes} without forcing/coercion -- same
 * strict style as builtins.c's prim_length (require the exact tag,
 * fatal otherwise), not real Nix's more permissive coerceToString. */
static void as_string(Value v, const char **bytes_out, int64_t *len_out, const char *who) {
    if (asnix_tag_of(v) != ASNIX_TAG_PTR) {
        fprintf(stderr, "asnix: %s: expected a string argument\n", who);
        exit(1);
    }
    const char *s = (const char *)(v.raw & ~0x7ULL);
    *len_out = *(const int64_t *)(s + ASNIX_STRING_OFF_LEN);
    *bytes_out = s + ASNIX_STRING_OFF_BYTES;
}

/*
 * ---------------------------------------------------------------------
 * Generalizing hashString's curry trick to arbitrary-domain arguments
 * ---------------------------------------------------------------------
 * builtins.c's hashString comment explains why a *finite enum* argument
 * (the algorithm name -- only 4 possible values) can be curried across
 * two separate arity-1 PrimOp calls just by returning one of a few
 * statically-known dedicated PrimOp function pointers: parse_app always
 * desugars `f a b c` into nested arity-1 App nodes, so OP_CALL only ever
 * sees argc=1 calls into a primop, and a primop's native fn signature
 * `Value (*)(const Value *args, int64_t argc)` has NO self/closure
 * pointer -- nothing lets one call's native fn pass data of its own
 * choosing into the *next* call's native fn.
 *
 * `substring start len str` (arity 3) and `concatStringsSep sep list`
 * (arity 2) need to carry `start`/`len`/`sep` across curry stages too,
 * but those are arbitrary ints/strings, not a 4-way enum -- there's no
 * finite set of dedicated function pointers to pick from this time.
 *
 * The fix: don't try to carry data between two *native* calls at all --
 * use the exact mechanism real curried *user* lambdas already use for
 * this (`a: b: c: body`), which is already fully general: OP_LAMBDA
 * captures "whatever Env is live right now" as the new Closure's `up`,
 * and OP_VAR_REF walks that chain back by De Bruijn depth. `substring`'s
 * top-level builtins-attrs entry is therefore not a PrimOp at all, but a
 * hand-built Closure chain -- built once, here, via the same IrBuilder
 * API the real compiler (src/c/compiler.c) uses for ordinary `a: b: ...`
 * syntax -- equivalent to compiling this fictitious Nix source:
 *
 *   start: len: str: __substring3 start len str
 *
 * where `__substring3` is a real, ordinary arity-3 PrimOp (never exposed
 * to user code directly -- nothing stops a 3-argument PrimOp existing,
 * OP_CALL's arity check works for any N; it's only the *parser* that
 * never generates an argc>1 call site for user-written application).
 * `make_curried_native` below builds exactly that chain for any arity
 * N>=2. No mutable global, no stashing extra data after the Closure/
 * PrimOp struct and recovering a "self" pointer some other way, no asm
 * changes -- just the same Closure/Env machinery every other curried
 * function in this evaluator already relies on, so it's automatically
 * safe under re-entrant native-to-Nix calls (e.g. a future `map
 * (builtins.substring 1 3) list`) the way a bespoke "self via adjacent-
 * stack-slot" or "global data stack" trick would not be.
 */
static Value make_curried_native(void *native_fn, int64_t arity) {
    Value native_primop = make_primop_value(native_fn, arity);
    /* Never a lambda-pattern -- `builtins.functionArgs` on any of these
     * closures is `{}`, same as real Nix's `functionArgs builtins.add`
     * (M37). M80: the shared canonical marker, not a fresh allocation --
     * see builtins_shared.h's asnix_bshared_native_formals_marker. */
    Value empty_formals = asnix_bshared_native_formals_marker();

    /* Innermost program: runs once the *last* curried argument has been
     * supplied, in the Env chain built up by `arity` nested single-slot
     * closure calls. Pushes the real native primop, then each of its
     * `arity` arguments in order by walking back up that chain (the
     * most-recently-bound param is depth 0, the first-bound one is
     * depth arity-1), then calls it for real. */
    IrBuilder *b = ib_new();
    ib_push_const(b, (int64_t)native_primop.raw);
    for (int64_t depth = arity - 1; depth >= 0; depth--) ib_var_ref(b, depth, 0);
    ib_call(b, arity);
    ib_ret(b);
    IrProgram *prog = ib_finish(b);

    /* Wrap `arity - 1` more times: each outer layer's entire body is
     * just "construct and return the next-inner layer's closure",
     * exactly what compile_expr's AST_LAMBDA case emits for each `x:`
     * in a real curried `a: b: c: ...` user lambda. */
    for (int64_t i = arity - 1; i >= 1; i--) {
        IrBuilder *wrap_b = ib_new();
        ib_lambda(wrap_b, 1, prog, (int64_t)empty_formals.raw);
        ib_ret(wrap_b);
        prog = ib_finish(wrap_b);
    }

    /* The outermost closure itself -- captures no Env (up = NULL, same
     * as any other root-scope value) since nothing has been bound yet. */
    void *mem = asnix_arena_alloc(ASNIX_CLOSURE_SIZE);
    *(void **)((char *)mem + ASNIX_CLOSURE_OFF_UP) = NULL;
    *(IrProgram **)((char *)mem + ASNIX_CLOSURE_OFF_PROG) = prog; /* M100: one pointer, not {ops,pool} */
    *(int64_t *)((char *)mem + ASNIX_CLOSURE_OFF_ARITY) = 1;
    *(uint64_t *)((char *)mem + ASNIX_CLOSURE_OFF_FORMALS) = empty_formals.raw;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_CLOSURE };
}

/* ---------------------------------------------------------------------
 * builtins.substring start len str (arity 3)
 * ---------------------------------------------------------------------
 * Verified against real `nix-instantiate --eval`:
 *   substring 1 3 "hello"    == "ell"    (ordinary slice)
 *   substring 1 100 "hello"  == "ello"   (len beyond end: clamp)
 *   substring 10 3 "hello"   == ""       (start beyond end: clamp to "")
 *   substring 5 3 "hello"    == ""       (start == length: clamp to "")
 *   substring 1 (-1) "hello" == "ello"   (negative len == "rest of string")
 *   substring (-1) 3 "hello" -> error "negative start position in 'substring'"
 */
static Value substring3_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value start_v = args[0], len_v = args[1], str_v = args[2];
    if (asnix_tag_of(start_v) != ASNIX_TAG_INT || asnix_tag_of(len_v) != ASNIX_TAG_INT)
        fatal("substring: start and len must be integers");
    int64_t start = asnix_int_value(start_v);
    int64_t len = asnix_int_value(len_v);
    if (start < 0) fatal("negative start position in 'substring'");

    const char *bytes;
    int64_t slen;
    as_string(str_v, &bytes, &slen, "substring");

    int64_t start_n = start > slen ? slen : start;
    int64_t actual_len = len < 0 ? slen - start_n : (start_n + len > slen ? slen - start_n : len);
    Value out = asnix_make_string_value(bytes + start_n, (size_t)actual_len);
    /* M80: real Nix's own `prim_substring` copies the WHOLE original
     * string's context onto the slice verbatim, regardless of which
     * bytes the slice actually keeps (confirmed directly: `getContext
     * (substring 0 0 "${d}")` is non-empty on real Nix, same as the
     * full string's -- substring never trims context). */
    const AsnixStringContext *ctx = asnix_string_context_of(str_v);
    if (!asnix_ctx_is_empty(ctx)) asnix_string_attach_context(out, ctx);
    return out;
}

static Value build_substring(void) {
    return make_curried_native((void *)substring3_impl, 3);
}

/* ---------------------------------------------------------------------
 * builtins.stringLength s (arity 1)
 * Verified: stringLength "hello" == 5, stringLength "" == 0.
 * --------------------------------------------------------------------- */
static Value prim_stringLength(const Value *args, int64_t argc) {
    (void)argc;
    const char *bytes;
    int64_t len;
    as_string(args[0], &bytes, &len, "stringLength");
    return asnix_make_int(len);
}

static Value build_stringLength(void) {
    return make_primop_value((void *)prim_stringLength, 1);
}

/* ---------------------------------------------------------------------
 * builtins.concatStringsSep sep list (arity 2). Note: real Nix has
 * `concatStringsSep` as a genuine builtin but *not* `concatStrings`
 * (that one's `lib.concatStrings = concatStringsSep ""`, nixpkgs-level,
 * not builtins-level) -- confirmed via `builtins ? concatStrings` ==
 * false and `builtins ? concatStringsSep` == true on a real
 * nix-instantiate, so only concatStringsSep is implemented here.
 * Verified: concatStringsSep ", " ["a" "b" "c"] == "a, b, c";
 * concatStringsSep ", " [] == "".
 * --------------------------------------------------------------------- */
static Value concatStringsSep2_impl(const Value *args, int64_t argc) {
    (void)argc;
    const char *sep_bytes;
    int64_t sep_len;
    as_string(args[0], &sep_bytes, &sep_len, "concatStringsSep");

    Value list_v = args[1];
    if (asnix_tag_of(list_v) != ASNIX_TAG_LIST) fatal("concatStringsSep: second argument must be a list");
    const char *list_base = (const char *)(list_v.raw & ~0x7ULL);
    int64_t count = *(const int64_t *)(list_base + ASNIX_LIST_OFF_COUNT);
    const Value *elems = (const Value *)(list_base + ASNIX_LIST_OFF_ELEMS);

    int64_t total = 0;
    /* M76/M77 follow-up, merged from two independent fixes found chasing
     * the d.nix/nixpkgs-hash-parity bug-chase:
     *  - each element is coerced the same way a genuine `"${...}"` string
     *    interpolation would (strings/paths/attrsets with `__toString`/
     *    `outPath` -- see asnix_coerce_to_string_for_concat_sep's own
     *    comment in value_util.c), NOT required to already be a bare
     *    string -- matches real Nix's `prim_concatStringsSep`, which
     *    calls `coerceToString` with its default (non-`coerceMore`)
     *    arguments on each raw list element. Found needed by nixpkgs's
     *    curl build: a `concatStringsSep`'d list there contains a
     *    derivation attrset element outright.
     *  - every element's own string context (if any -- including any
     *    NEW context the coercion above just attached, e.g. a bare
     *    already-in-store Path element) is unioned into the joined
     *    result's context, same pattern as value_util.c's
     *    `concat_strings_impl` and the ASNIX_TAG_LIST branch of
     *    `asnix_coerce_to_string`. Without this, `lib.makeBinPath`/
     *    `lib.makeSearchPath` (nixpkgs-level, `concatStringsSep ":"
     *    (map (p: "${p}/bin") pkgs)`) silently drop every referenced
     *    derivation's context, so a `derivation {...}` attribute built
     *    that way (e.g. a bootstrap derivation's `PATH` env var) is
     *    missing those derivations from its `InputDrvs` entirely --
     *    confirmed: asnix's own `bootstrap-coreutils-5.0.drv` had only 2
     *    of the real 7 `InputDrvs` entries, the missing 5 appearing
     *    ONLY inside its `PATH` var's `makeBinPath`-joined string.
     * Coerced once per element up front into a scratch array so the two
     * passes below (length-sum, then copy) don't re-run any
     * `__toString` call twice. */
    AsnixStringContext ctx;
    asnix_ctx_init(&ctx);
    Value *forced = malloc((size_t)(count > 0 ? count : 1) * sizeof(Value));
    for (int64_t i = 0; i < count; i++) {
        const char *b;
        int64_t l;
        forced[i] = asnix_coerce_to_string_for_concat_sep(asnix_force_for_print(elems[i]));
        as_string(forced[i], &b, &l, "concatStringsSep");
        asnix_ctx_union_into(&ctx, asnix_string_context_of(forced[i]));
        total += l;
        if (i + 1 < count) total += sep_len;
    }

    char *out = asnix_arena_alloc((uint64_t)total);
    char *cur = out;
    for (int64_t i = 0; i < count; i++) {
        const char *b;
        int64_t l;
        as_string(forced[i], &b, &l, "concatStringsSep");
        memcpy(cur, b, (size_t)l);
        cur += l;
        if (i + 1 < count) {
            memcpy(cur, sep_bytes, (size_t)sep_len);
            cur += sep_len;
        }
    }
    free(forced);
    Value result = asnix_make_string_value(out, (size_t)total);
    asnix_string_attach_context(result, &ctx);
    return result;
}

static Value build_concatStringsSep(void) {
    return make_curried_native((void *)concatStringsSep2_impl, 2);
}

/* ---------------------------------------------------------------------
 * builtins.addErrorContext msg value (M39, `__addErrorContext` in real
 * Nix -- builtins-only, confirmed in ~/nix/src/libexpr/primops.cc). Real
 * Nix's only observable effect is annotating `value`'s error trace with
 * `msg` *if and only if* forcing `value` throws -- confirmed `msg` isn't
 * forced any more eagerly than `value` itself by ordinary call-by-need
 * (`let y = addErrorContext "ctx" (abort "x"); in 1` == 1, same as any
 * other unused lazy binding; `addErrorContext "ctx" (abort "x")`
 * forced directly DOES abort, exactly as if written bare -- no special
 * extra laziness beyond normal application). Since asnix has no error-
 * trace/position system at all (no position tracking anywhere in this
 * codebase), there is nothing to attach `msg` to -- implemented as a
 * pure pass-through, `msg: value: value`, which already reproduces
 * every observable success/failure case above via this project's
 * ordinary M25 call-by-need semantics (no NOFORCE needed: forcing only
 * happens when the *caller* forces the call's own result, same as real
 * Nix). */
static Value addErrorContext2_impl(const Value *args, int64_t argc) {
    (void)argc;
    return args[1];
}
static Value build_addErrorContext(void) {
    return make_curried_native((void *)addErrorContext2_impl, 2);
}

/* ---------------------------------------------------------------------
 * Type predicates. All verified true/false the obvious way against real
 * nix-instantiate (isNull null/isNull 1, isString "a"/isString 1,
 * isList [1 2 3], isAttrs {a=1;}, isFunction (a:a)/isFunction
 * builtins.length, isInt 1/isInt 1.5, isBool true) -- 1.5 isn't
 * representable in this evaluator's INT-only numeric tower yet, so
 * isInt has no real "false on a float" case to check here, only "true
 * on an int". isFunction is true for both a user Closure and a native
 * PrimOp (including a partially-applied curried one, e.g.
 * `isFunction (builtins.substring 1)` == true on real Nix), matching
 * this file's own build_substring/build_concatStringsSep returning
 * ASNIX_TAG_CLOSURE values -- typeOf already treats CLOSURE and PRIMOP
 * identically ("lambda") in builtins.c, so this is consistent.
 * --------------------------------------------------------------------- */
static Value prim_isNull(const Value *args, int64_t argc) {
    (void)argc;
    return asnix_make_imm(asnix_tag_of(args[0]) == ASNIX_TAG_IMM && asnix_imm_subtag(args[0]) == ASNIX_IMM_NULL
                               ? ASNIX_IMM_TRUE
                               : ASNIX_IMM_FALSE);
}
static Value prim_isString(const Value *args, int64_t argc) {
    (void)argc;
    /* Paths and strings share ASNIX_TAG_PTR (see heap.h) -- isString must
     * exclude paths (found via d.nix: `builtins.isString ./.` wrongly
     * returned true here; real Nix returns false). M51: a Float is a
     * THIRD such kind -- excluded the same way (verified against
     * `nix-instantiate --eval`: `isString 1.5` is false). */
    return asnix_make_imm(asnix_tag_of(args[0]) == ASNIX_TAG_PTR && !asnix_value_is_path(args[0]) &&
                                   !asnix_value_is_float(args[0])
                               ? ASNIX_IMM_TRUE
                               : ASNIX_IMM_FALSE);
}
static Value prim_isPath(const Value *args, int64_t argc) {
    (void)argc;
    return asnix_make_imm(asnix_tag_of(args[0]) == ASNIX_TAG_PTR && asnix_value_is_path(args[0])
                               ? ASNIX_IMM_TRUE
                               : ASNIX_IMM_FALSE);
}
/* `builtins.isFloat` (M51) -- verified against `nix-instantiate --eval`:
 * `isFloat 1.2` is true, `isFloat (1 + 1.0)` is true (the `+` operator's
 * own int/float promotion, see op_arith.S/value_util.c), `isFloat 1` is
 * false. */
static Value prim_isFloat(const Value *args, int64_t argc) {
    (void)argc;
    return asnix_make_imm(asnix_value_is_float(args[0]) ? ASNIX_IMM_TRUE : ASNIX_IMM_FALSE);
}
static Value prim_isList(const Value *args, int64_t argc) {
    (void)argc;
    return asnix_make_imm(asnix_tag_of(args[0]) == ASNIX_TAG_LIST ? ASNIX_IMM_TRUE : ASNIX_IMM_FALSE);
}
static Value prim_isAttrs(const Value *args, int64_t argc) {
    (void)argc;
    return asnix_make_imm(asnix_tag_of(args[0]) == ASNIX_TAG_ATTRS ? ASNIX_IMM_TRUE : ASNIX_IMM_FALSE);
}
static Value prim_isFunction(const Value *args, int64_t argc) {
    (void)argc;
    int tag = asnix_tag_of(args[0]);
    return asnix_make_imm(tag == ASNIX_TAG_CLOSURE || tag == ASNIX_TAG_PRIMOP ? ASNIX_IMM_TRUE : ASNIX_IMM_FALSE);
}

/* `builtins.functionArgs` (M37, `__functionArgs` in real Nix -- builtins-
 * only, confirmed in ~/nix/src/libexpr/primops.cc): every Closure already
 * carries its pre-built result as ASNIX_CLOSURE_OFF_FORMALS (set once at
 * compile time, compiler.c's AST_LAMBDA case -- see ir.h's comment on
 * that field for why it's precomputed rather than derived here). A
 * PrimOp (or anything else -- real Nix's own version just requires a
 * function argument, but asnix hasn't implemented argument-type
 * checking that strictly here) has no such field at all; matching real
 * Nix's `functionArgs builtins.add` == `{}`. */
static Value prim_functionArgs(const Value *args, int64_t argc) {
    (void)argc;
    if (asnix_tag_of(args[0]) != ASNIX_TAG_CLOSURE) return asnix_bshared_make_attrs(NULL, NULL, 0);
    const char *base = (const char *)(args[0].raw & ~0x7ULL);
    return *(const Value *)(base + ASNIX_CLOSURE_OFF_FORMALS);
}
static Value prim_isInt(const Value *args, int64_t argc) {
    (void)argc;
    return asnix_make_imm(asnix_tag_of(args[0]) == ASNIX_TAG_INT ? ASNIX_IMM_TRUE : ASNIX_IMM_FALSE);
}
static Value prim_isBool(const Value *args, int64_t argc) {
    (void)argc;
    if (asnix_tag_of(args[0]) != ASNIX_TAG_IMM) return asnix_make_imm(ASNIX_IMM_FALSE);
    int sub = asnix_imm_subtag(args[0]);
    return asnix_make_imm(sub == ASNIX_IMM_TRUE || sub == ASNIX_IMM_FALSE ? ASNIX_IMM_TRUE : ASNIX_IMM_FALSE);
}

/* `builtins.floor`/`builtins.ceil` (M51). Real Nix (~/nix's primops.cc
 * `prim_floor`/`prim_ceil`) accepts either an int or a float and always
 * returns an INT: an int argument passes through completely unmodified
 * (never even round-tripped through a double, which would risk precision
 * loss for a huge int -- not that this matters much given asnix's own
 * pre-existing 60-bit int range, but it's what real Nix does and it's
 * free to match); a float argument is floor()'d/ceil()'d and truncated
 * to int64. Implemented via plain casts rather than libm's floor()/
 * ceil() (`(int64_t)d` already truncates toward zero for a finite
 * double, same as C's own int conversion -- floor is "truncate toward
 * zero, then step one more negative if that truncated anything off a
 * negative value"; ceil is the positive-side mirror of the same rule),
 * so this needs no new link dependency. Verified against
 * numtide/nix-conformance's eval-okay-floor-ceil.nix: `floor 23.5` ==
 * 23, `ceil 23.5` == 24, `floor 23` == 23, `ceil 23` == 23 (note: ALL
 * FOUR results print as plain ints via `toString`, confirming these
 * really do return INT, not a float that happens to look integral). Out
 * of range (NixInt overflow) is not specially handled, matching this
 * project's existing documented 60-bit-int-range scope cut elsewhere. */
static Value prim_floor(const Value *args, int64_t argc) {
    (void)argc;
    Value v = args[0];
    if (asnix_tag_of(v) == ASNIX_TAG_INT) return v;
    if (!asnix_value_is_float(v)) fatal("floor: argument is not a number");
    double d = asnix_float_value(v);
    int64_t t = (int64_t)d; /* truncates toward zero */
    if (d < 0 && (double)t != d) t -= 1;
    return asnix_make_int(t);
}
static Value prim_ceil(const Value *args, int64_t argc) {
    (void)argc;
    Value v = args[0];
    if (asnix_tag_of(v) == ASNIX_TAG_INT) return v;
    if (!asnix_value_is_float(v)) fatal("ceil: argument is not a number");
    double d = asnix_float_value(v);
    int64_t t = (int64_t)d; /* truncates toward zero */
    if (d > 0 && (double)t != d) t += 1;
    return asnix_make_int(t);
}

/* `baseNameOf`/`dirOf` (M23) -- two of real Nix's bare root-scope globals
 * (see builtins.h's asnix_make_import_primop comment for the full
 * curated list), found needed by numtide/nix-conformance's
 * eval-okay-baseNameOf.nix/eval-okay-builtins-dirOf.nix/eval-okay-path.nix
 * and others, all of which call these bare, not `builtins.X`. Ported
 * directly from real Nix's own `prim_baseNameOf`/`prim_dirOf`
 * (~/nix/src/libexpr/primops.cc), verified against `nix-instantiate
 * --eval` for every edge case below:
 *   baseNameOf "/foo/bar.nix"  -> "bar.nix"      (always a plain string,
 *   baseNameOf ./foo/bar.nix   -> "bar.nix"       regardless of whether
 *   baseNameOf "foo/"          -> "foo"           the input was a path)
 *   dirOf "/foo/bar.nix"       -> "/foo"          (type-preserving: a
 *   dirOf ./foo/bar.nix        -> /foo (a Path)    Path in, Path out;
 *   dirOf "foo"                -> "."              a String in,
 *   dirOf "/"                  -> "/"              String out)
 * Scope cut: real Nix accepts (and coerces) any string-coercible type
 * here (further erroring with real Nix's own wording on int/bool/null);
 * asnix only accepts an actual TAG_PTR (String or Path) argument, fatal
 * otherwise -- narrower, but every real caller found so far only ever
 * passes an actual path/string. */
static Value prim_baseNameOf(const Value *args, int64_t argc) {
    (void)argc;
    int64_t len;
    const char *bytes = asnix_ptr_bytes(args[0], &len);
    if (len > 0 && bytes[len - 1] == '/') len--; /* AT MOST ONE trailing slash stripped (an
                                                    * `if`, not a `while`) -- matching real
                                                    * Nix's legacyBaseNameOf exactly (NOT GNU
                                                    * basename's "any number"): verified
                                                    * `baseNameOf "a//"` => "" (strip one
                                                    * slash -> "a/", then the segment after
                                                    * ITS last '/' is empty), not "a" -- a
                                                    * prior version of this line used `while`,
                                                    * silently stripping every trailing slash
                                                    * instead of just one, found via
                                                    * numtide/nix-conformance's
                                                    * eval-okay-baseNameOf.nix. */
    int64_t start = len;
    while (start > 0 && bytes[start - 1] != '/') start--;
    Value out = asnix_make_string_value(bytes + start, (size_t)(len - start));
    /* M80: real Nix's own `prim_baseNameOf` copies `args[0]`'s context onto
     * the result verbatim (`v.mkString(getName(str), context)`,
     * ~/nix/src/libexpr/primops.cc) -- confirmed needed directly:
     * `getContext (baseNameOf "${d}")` keeps `d`'s own context on real
     * Nix. `asnix_string_context_of` on a Path is always NULL (Paths
     * never get an entry), so this is a no-op for the Path-argument case. */
    const AsnixStringContext *ctx = asnix_string_context_of(args[0]);
    if (!asnix_ctx_is_empty(ctx)) asnix_string_attach_context(out, ctx);
    return out;
}

static Value prim_dirOf(const Value *args, int64_t argc) {
    (void)argc;
    Value v = args[0];
    int64_t len;
    const char *bytes = asnix_ptr_bytes(v, &len);
    int is_path = asnix_value_is_path(v);
    /* asnix Paths are always already-resolved absolute paths with no
     * trailing slash (see compiler.c's AST_PATH/resolve_path_literal),
     * so "/" is the only case needing the real-Nix "isRoot, keep as-is"
     * special case -- everything else always contains a '/' to split on. */
    if (is_path) {
        if (len == 1 && bytes[0] == '/') return asnix_make_path_value("/", 1);
        int64_t pos = len;
        while (pos > 0 && bytes[pos - 1] != '/') pos--;
        pos--; /* back up onto the '/' itself */
        if (pos <= 0) return asnix_make_path_value("/", 1);
        return asnix_make_path_value(bytes, (size_t)pos);
    }
    int64_t pos = len;
    while (pos > 0 && bytes[pos - 1] != '/') pos--;
    /* M80: the String-argument branches, unlike the Path ones above,
     * propagate `v`'s own context onto the result -- a Path value never
     * carries string context at all in real Nix (context only applies
     * to strings-with-context), so only this branch needs it. */
    const AsnixStringContext *ctx = asnix_string_context_of(v);
    if (pos == 0) {
        Value out = asnix_make_string_value(".", 1);
        if (!asnix_ctx_is_empty(ctx)) asnix_string_attach_context(out, ctx);
        return out;
    }
    pos--; /* back up onto the '/' itself */
    if (pos == 0) {
        Value out = asnix_make_string_value("/", 1);
        if (!asnix_ctx_is_empty(ctx)) asnix_string_attach_context(out, ctx);
        return out;
    }
    Value out = asnix_make_string_value(bytes, (size_t)pos);
    if (!asnix_ctx_is_empty(ctx)) asnix_string_attach_context(out, ctx);
    return out;
}

static Value build_baseNameOf(void) { return make_primop_value((void *)prim_baseNameOf, 1); }
static Value build_dirOf(void) { return make_primop_value((void *)prim_dirOf, 1); }
Value asnix_make_baseNameOf_primop(void) { return make_primop_value((void *)prim_baseNameOf, 1); }
Value asnix_make_dirOf_primop(void) { return make_primop_value((void *)prim_dirOf, 1); }

/* `builtins.placeholder` (M44) -- one of real Nix's bare root-scope
 * globals (confirmed in `~/nix/src/libexpr/primops.cc`: `.name =
 * "placeholder"`, no `__` prefix). Computes an output placeholder
 * string for a derivation output name, ported directly from
 * `~/nix/src/libstore/derivations.cc`'s `hashPlaceholder`: `"/" +
 * base32(sha256("nix-output:" + outputName))` (Nix's own base32
 * variant, no hash-type prefix) -- reuses this project's existing
 * `asnix_hash_sha256`/`asnix_base32_encode` (already relied on for
 * store-path hashing, M8) rather than adding any new hashing
 * machinery. Verified against real `nix-instantiate --eval`:
 * `builtins.placeholder "out"` ==
 * "/1rz4g4znpzjwh1xymhjpm42vipw92pr73vdgl6xs1hycac8kf2n9". */
static Value prim_placeholder(const Value *args, int64_t argc) {
    (void)argc;
    ASNIX_REQUIRE_STRING(args[0], "placeholder");
    int64_t name_len;
    const char *name_bytes;
    asnix_bshared_string_bytes(args[0], &name_bytes, &name_len, "placeholder");

    size_t preimage_len = 11 + (size_t)name_len; /* "nix-output:" */
    char *preimage = malloc(preimage_len);
    memcpy(preimage, "nix-output:", 11);
    memcpy(preimage + 11, name_bytes, (size_t)name_len);

    uint8_t digest[ASNIX_HASH_SHA256_LEN];
    int64_t rc = asnix_hash_sha256(preimage, preimage_len, digest);
    free(preimage);
    if (rc != 0) {
        fprintf(stderr, "asnix: placeholder: failed to load libcrypto (rc=%lld) -- enter the flake devShell\n",
                (long long)rc);
        exit(1);
    }

    size_t b32_len = asnix_base32_encoded_length(ASNIX_HASH_SHA256_LEN);
    char *out = malloc(1 + b32_len);
    out[0] = '/';
    asnix_base32_encode(digest, ASNIX_HASH_SHA256_LEN, out + 1);
    Value result = asnix_make_string_value(out, 1 + b32_len);
    free(out);
    return result;
}
static Value build_placeholder(void) { return make_primop_value((void *)prim_placeholder, 1); }
Value asnix_make_placeholder_primop(void) { return make_primop_value((void *)prim_placeholder, 1); }

/* `builtins.toFile` (M45, `__toFile` in real Nix -- builtins-only,
 * confirmed in `~/nix/src/libexpr/primops.cc`). Computes the store
 * path a `name`/`contents` pair would get, ported directly from
 * `~/nix/src/libstore/store-dir-config.cc`'s `makeFixedOutputPathFromCA`
 * TextInfo case: `makeStorePath(makeType(store, "text", refs), sha256
 * (contents), name)` -- `makeType` only appends anything to the `"text"`
 * type string when `contents` has references to OTHER store paths
 * (string context), which this project doesn't track at all (confirmed:
 * no context/dependency machinery anywhere in this codebase), so the
 * type string here is always exactly `"text"` -- the common case, and
 * the only one asnix's `contents` strings (always context-free) can
 * ever actually need. This is the exact same `type:sha256:<storeDir>:
 * <name>`-preimage algorithm this project's own `derivation.c` already
 * uses for a `.drv` file's own path (also a `"text"`-type hash) --
 * reuses `store_path.h`'s public preimage/path-assembly helpers
 * directly rather than duplicating derivation.c's file-local
 * `compute_store_path` wrapper. Does NOT perform real Nix's actual
 * filesystem side effect (writing `contents` into `/nix/store/...`) --
 * this project has no real content-addressed store at all (a
 * separately tracked, larger scope cut); only the resulting path
 * *string* is computed, matching what every reachable use in this
 * project's own evaluation (string manipulation on the result, never
 * an actual build) needs. Verified against real `nix-instantiate
 * --eval`: `builtins.toFile "hello.txt" "Hello World"` ==
 * "/nix/store/5f20vz4zb4xixz2yrbyr7q8m8xcz0jzf-hello.txt". */
static Value prim_toFile(const Value *args, int64_t argc) {
    (void)argc;
    ASNIX_REQUIRE_STRING(args[0], "toFile");
    ASNIX_REQUIRE_STRING(args[1], "toFile");

    char *name = asnix_string_value_to_cstr(args[0]);
    asnix_check_store_path_name(name);

    int64_t contents_len;
    const char *contents_bytes;
    asnix_bshared_string_bytes(args[1], &contents_bytes, &contents_len, "toFile");

    /* M80: `contents`' own context becomes this file's STORE REFERENCES
     * -- ported from `~/nix/src/libexpr/primops.cc`'s `prim_toFile`:
     * every Opaque context element is a reference (`refs.insert(p->
     * path)`); any OTHER kind (Built/DrvDeep -- i.e. `contents`
     * mentions a derivation, not just a plain path) is a hard error
     * ("files created by builtins.toFile may not reference
     * derivations"), confirmed directly against real `nix-instantiate
     * --eval` (`eval-fail-drv-tofile-output-reference.nix`: `toFile "t"
     * "${d}"` fails for exactly this reason -- a derivation OUTPUT
     * reference, not a plain path). References (if any) get folded into
     * the preimage's "type" field via real Nix's `makeType`
     * (`~/nix/src/libstore/store-dir-config.cc`): `"text" + for each
     * reference, SORTED ascending, ":" + path` -- changes the resulting
     * store path's hash, confirmed needed directly:
     * `eval-okay-drv-sources.nix`'s `refers = toFile "refers" "see
     * ${text}";` must hash to the exact same path real Nix computes,
     * which depends on `text`'s own store path being folded into this
     * preimage, not just `contents`' raw bytes. */
    const AsnixStringContext *contents_ctx = asnix_string_context_of(args[1]);
    if (contents_ctx && (contents_ctx->ndrvs > 0 || contents_ctx->ndeep > 0)) {
        fprintf(stderr,
                "asnix: toFile: files created by builtins.toFile may not reference derivations, but '%s' does\n",
                name);
        exit(1);
    }
    size_t nrefs = contents_ctx ? contents_ctx->nsrcs : 0;
    char **refs = NULL;
    if (nrefs > 0) {
        refs = malloc(nrefs * sizeof(char *));
        memcpy(refs, contents_ctx->srcs, nrefs * sizeof(char *));
        /* Ascending lexicographic sort -- see this function's own
         * comment above (matches real Nix's `StorePathSet`, a
         * `std::set<StorePath>` ordered by its own basename string). */
        for (size_t i = 1; i < nrefs; i++) {
            size_t j = i;
            while (j > 0 && strcmp(refs[j - 1], refs[j]) > 0) {
                char *tmp = refs[j - 1];
                refs[j - 1] = refs[j];
                refs[j] = tmp;
                j--;
            }
        }
    }

    uint8_t inner_digest[ASNIX_HASH_SHA256_LEN];
    int64_t rc = asnix_hash_sha256(contents_bytes, (size_t)contents_len, inner_digest);
    if (rc != 0) {
        fprintf(stderr, "asnix: toFile: failed to load libcrypto (rc=%lld) -- enter the flake devShell\n",
                (long long)rc);
        exit(1);
    }

    static const char hexdigits[] = "0123456789abcdef";
    char hashstr[7 + 64 + 1];
    memcpy(hashstr, "sha256:", 7);
    for (int i = 0; i < ASNIX_HASH_SHA256_LEN; i++) {
        hashstr[7 + 2 * i] = hexdigits[inner_digest[i] >> 4];
        hashstr[7 + 2 * i + 1] = hexdigits[inner_digest[i] & 0xf];
    }
    hashstr[7 + 64] = '\0';

    char *type_str = malloc(4 + nrefs * 300 + 1);
    size_t type_len = 4;
    memcpy(type_str, "text", 4);
    for (size_t i = 0; i < nrefs; i++) {
        type_str[type_len++] = ':';
        size_t rl = strlen(refs[i]);
        memcpy(type_str + type_len, refs[i], rl);
        type_len += rl;
    }
    type_str[type_len] = '\0';

    char preimage[4096];
    size_t n = asnix_store_path_preimage(type_str, hashstr, name, preimage, sizeof(preimage));
    free(type_str);
    free(refs);
    if (n == 0) {
        fprintf(stderr, "asnix: toFile: preimage too long\n");
        exit(1);
    }

    uint8_t outer_digest[ASNIX_HASH_SHA256_LEN];
    rc = asnix_hash_sha256(preimage, n, outer_digest);
    if (rc != 0) {
        fprintf(stderr, "asnix: toFile: failed to load libcrypto (rc=%lld) -- enter the flake devShell\n",
                (long long)rc);
        exit(1);
    }

    char out_path[300];
    if (asnix_make_store_path_from_digest(outer_digest, name, out_path, sizeof(out_path)) == 0) {
        fprintf(stderr, "asnix: toFile: computed store path too long\n");
        exit(1);
    }
    Value out = asnix_make_string_value(out_path, strlen(out_path));
    /* M76: real Nix's own `toFile` (`mkStorePathString`,
     * `~/nix/src/libexpr/eval.cc`) attaches `Opaque(storePath)` --
     * ITS OWN output path -- as the result string's context, NOT any
     * context collected from `contents` (that's folded into the type
     * string / NAR references instead, a separate, still out-of-scope
     * nuance this project doesn't implement -- see this function's own
     * header comment). Found needed chasing d.nix past M76's core
     * mechanism: `<nix/fetchurl.nix>`'s own `mirrorsListFile = toFile
     * ...;` is exactly this pattern -- without this, the resulting
     * path reaches `derivation {...}` as a plain opaque string with no
     * trace of being a store reference at all, so it never made it
     * into `inputSrcs`. */
    AsnixStringContext ctx;
    asnix_ctx_init(&ctx);
    asnix_ctx_add_opaque(&ctx, out_path);
    asnix_string_attach_context(out, &ctx);
    return out;
}
static Value build_toFile(void) { return make_curried_native((void *)prim_toFile, 2); }

static Value build_isNull(void) { return make_primop_value((void *)prim_isNull, 1); }

/* `isNull` is one of real Nix's bare root-scope globals (verified against
 * `nix-instantiate --eval`, same research as M18's import/map/abort/
 * toString -- see docs/architecture.md). Wraps the exact same `prim_isNull`
 * `builtins.isNull` itself uses. */
Value asnix_make_isNull_primop(void) { return make_primop_value((void *)prim_isNull, 1); }
static Value build_isString(void) { return make_primop_value((void *)prim_isString, 1); }
static Value build_isPath(void) { return make_primop_value((void *)prim_isPath, 1); }
static Value build_functionArgs(void) { return make_primop_value((void *)prim_functionArgs, 1); }
static Value build_isList(void) { return make_primop_value((void *)prim_isList, 1); }
static Value build_isAttrs(void) { return make_primop_value((void *)prim_isAttrs, 1); }
static Value build_isFunction(void) { return make_primop_value((void *)prim_isFunction, 1); }
static Value build_isInt(void) { return make_primop_value((void *)prim_isInt, 1); }
static Value build_isBool(void) { return make_primop_value((void *)prim_isBool, 1); }
static Value build_isFloat(void) { return make_primop_value((void *)prim_isFloat, 1); }
static Value build_floor(void) { return make_primop_value((void *)prim_floor, 1); }
static Value build_ceil(void) { return make_primop_value((void *)prim_ceil, 1); }

/* `builtins.nixVersion` -- a plain string constant, not a function (real
 * Nix's is a build-time-baked version string, not computed). This
 * `build()` slot doesn't require a PrimOp: it's called once, at
 * `builtins`-attrset construction time, and whatever Value it returns is
 * stored directly -- no wrapping needed for a non-callable constant.
 * Reports the real Nix compat version installed on this dev machine
 * (`nix-instantiate --version`), comfortably above any `>= "2.18"`-style
 * nixpkgs/lib version gate. */
static Value build_nixVersion(void) { return asnix_make_string_value("2.35.2", 6); }

/* `builtins.currentSystem` -- same "plain constant, no PrimOp needed"
 * shape as nixVersion above. Real Nix registers this internally as
 * `__currentSystem` (confirmed in ~/nix/src/libexpr/primops.cc), i.e.
 * NOT a bare global (M27's bare-global rule: only non-`__`-prefixed
 * names are exposed bare) -- `builtins.currentSystem` only, matching
 * `nix-instantiate --eval -E 'currentSystem'` => "undefined variable".
 * Hardcoded to this project's one supported platform (x86_64-linux,
 * see flake.nix's `systems` list) rather than computed from `uname` --
 * there's nothing else this process could ever be running on. Found
 * needed evaluating real nixpkgs (`d.nix`'s default.nix chain selects
 * `.currentSystem` while resolving the default platform). */
static Value build_currentSystem(void) { return asnix_make_string_value("x86_64-linux", 12); }

/* `builtins.storeDir` (M71) -- same "plain constant, no PrimOp needed"
 * shape as nixVersion/currentSystem above. Real Nix's is the configured
 * Nix store's root directory (`/nix/store` on every real installation,
 * including this dev machine's -- confirmed via `nix-instantiate --eval
 * -E 'builtins.storeDir'`), `builtins`-only (confirmed: bare `storeDir`
 * is "undefined variable" there too, matching the same M27 bare-global
 * rule nixVersion/currentSystem already follow). Found via `d.nix`:
 * several nixpkgs packages (under `pkgs/by-name`) reference
 * `builtins.storeDir` directly, expecting a path-prefix-shaped string. */
static Value build_storeDir(void) { return asnix_make_string_value("/nix/store", 10); }

const AsnixBuiltinMiscDef ASNIX_BUILTINS_MISC[] = {
    { "substring", build_substring },
    { "stringLength", build_stringLength },
    { "concatStringsSep", build_concatStringsSep },
    { "isNull", build_isNull },
    { "isString", build_isString },
    { "isPath", build_isPath },
    { "functionArgs", build_functionArgs },
    { "addErrorContext", build_addErrorContext },
    { "isList", build_isList },
    { "isAttrs", build_isAttrs },
    { "isFunction", build_isFunction },
    { "isInt", build_isInt },
    { "isBool", build_isBool },
    { "isFloat", build_isFloat },
    { "floor", build_floor },
    { "ceil", build_ceil },
    { "nixVersion", build_nixVersion },
    { "currentSystem", build_currentSystem },
    { "storeDir", build_storeDir },
    { "baseNameOf", build_baseNameOf },
    { "dirOf", build_dirOf },
    { "placeholder", build_placeholder },
    { "toFile", build_toFile },
};

const size_t ASNIX_BUILTINS_MISC_COUNT = sizeof(ASNIX_BUILTINS_MISC) / sizeof(ASNIX_BUILTINS_MISC[0]);
