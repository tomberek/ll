/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "builtins_misc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ll/arena.h"
#include "ll/heap.h"
#include "ll/ir.h"
#include "ll/irbuild.h"
#include "base32.h"
#include "builtins_shared.h"
#include "hash_shim.h"
#include "store_path.h"
#include "string_context.h"
#include "value_util.h"

static Value make_primop_value(void *fn, int64_t arity) {
    void *mem = ll_arena_alloc(LL_PRIMOP_SIZE);
    *(void **)((char *)mem + LL_PRIMOP_OFF_FN) = fn;
    *(int64_t *)((char *)mem + LL_PRIMOP_OFF_ARITY) = arity;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | LL_TAG_PRIMOP };
}

static void fatal(const char *msg) {
    fprintf(stderr, "ll: %s\n", msg);
    exit(1);
}

static void as_string(Value v, const char **bytes_out, int64_t *len_out, const char *who) {
    if (ll_tag_of(v) != LL_TAG_PTR) {
        fprintf(stderr, "ll: %s: expected a string argument\n", who);
        exit(1);
    }
    const char *s = (const char *)(v.raw & ~0x7ULL);
    *len_out = *(const int64_t *)(s + LL_STRING_OFF_LEN);
    *bytes_out = s + LL_STRING_OFF_BYTES;
}

/*
 * make_curried_native generalizes hashString's curry trick (returning a
 * statically-known PrimOp function pointer) to arbitrary-domain
 * arguments like substring's start/len or concatStringsSep's sep, which
 * aren't a small finite enum. A PrimOp's native fn signature `Value
 * (*)(const Value *args, int64_t argc)` has no self/closure pointer, so
 * nothing lets one call's native fn pass data of its own choosing into
 * the next call's native fn.
 *
 * Instead this builds a real Closure chain -- the same OP_LAMBDA/
 * OP_VAR_REF Env-capture mechanism a user-written curried lambda
 * `a: b: c: body` already uses -- via the same IrBuilder API the real
 * compiler (src/c/compiler.c) uses, equivalent to compiling this
 * fictitious Nix source:
 *
 *   start: len: str: __substring3 start len str
 *
 * where `__substring3` is an ordinary arity-3 PrimOp, never exposed to
 * user code directly. `make_curried_native` builds exactly that chain
 * for any arity N>=2.
 */
static Value make_curried_native(void *native_fn, int64_t arity) {
    Value native_primop = make_primop_value(native_fn, arity);
    /* The shared canonical empty-formals marker, not a fresh allocation
     * -- see builtins_shared.h's ll_bshared_native_formals_marker. Keeps
     * `functionArgs` on any of these closures == {}, matching real
     * Nix's `functionArgs builtins.add` == {}. */
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
    *(void **)((char *)mem + LL_CLOSURE_OFF_UP) = NULL;
    *(IrProgram **)((char *)mem + LL_CLOSURE_OFF_PROG) = prog;
    *(int64_t *)((char *)mem + LL_CLOSURE_OFF_ARITY) = 1;
    *(uint64_t *)((char *)mem + LL_CLOSURE_OFF_FORMALS) = empty_formals.raw;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | LL_TAG_CLOSURE };
}

static Value substring3_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value start_v = args[0], len_v = args[1], str_v = args[2];
    if (ll_tag_of(start_v) != LL_TAG_INT || ll_tag_of(len_v) != LL_TAG_INT)
        fatal("substring: start and len must be integers");
    int64_t start = ll_int_value(start_v);
    int64_t len = ll_int_value(len_v);
    if (start < 0) fatal("negative start position in 'substring'");

    const char *bytes;
    int64_t slen;
    as_string(str_v, &bytes, &slen, "substring");

    int64_t start_n = start > slen ? slen : start;
    int64_t actual_len = len < 0 ? slen - start_n : (start_n + len > slen ? slen - start_n : len);
    Value out = ll_make_string_value(bytes + start_n, (size_t)actual_len);
    /* substring copies the WHOLE original string's context onto the
     * slice verbatim, regardless of which bytes the slice actually
     * keeps -- matches real Nix's prim_substring (substring never
     * trims context). */
    const LlStringContext *ctx = ll_string_context_of(str_v);
    if (!ll_ctx_is_empty(ctx)) ll_string_attach_context(out, ctx);
    return out;
}

static Value build_substring(void) {
    return make_curried_native((void *)substring3_impl, 3);
}

static Value prim_stringLength(const Value *args, int64_t argc) {
    (void)argc;
    const char *bytes;
    int64_t len;
    as_string(args[0], &bytes, &len, "stringLength");
    return ll_make_int(len);
}

static Value build_stringLength(void) {
    return make_primop_value((void *)prim_stringLength, 1);
}

static Value concatStringsSep2_impl(const Value *args, int64_t argc) {
    (void)argc;
    const char *sep_bytes;
    int64_t sep_len;
    as_string(args[0], &sep_bytes, &sep_len, "concatStringsSep");

    Value list_v = args[1];
    if (ll_tag_of(list_v) != LL_TAG_LIST) fatal("concatStringsSep: second argument must be a list");
    const char *list_base = (const char *)(list_v.raw & ~0x7ULL);
    int64_t count = *(const int64_t *)(list_base + LL_LIST_OFF_COUNT);
    const Value *elems = (const Value *)(list_base + LL_LIST_OFF_ELEMS);

    int64_t total = 0;
    /* Each element is coerced the same way a genuine `"${...}"` string
     * interpolation would (strings/paths/attrsets with `__toString`/
     * `outPath`), not required to already be a bare string -- matches
     * real Nix's `prim_concatStringsSep`. Each element's own string
     * context (including any new context the coercion just attached) is
     * unioned into the joined result's context; without this,
     * `lib.makeBinPath`/`lib.makeSearchPath`-style joins silently drop
     * every referenced derivation's context, so a `derivation {...}`
     * built from the result is missing those derivations from its
     * `InputDrvs`.
     * Coerced once per element up front into a scratch array so the two
     * passes below (length-sum, then copy) don't re-run any
     * `__toString` call twice. */
    LlStringContext ctx;
    ll_ctx_init(&ctx);
    Value *forced = malloc((size_t)(count > 0 ? count : 1) * sizeof(Value));
    for (int64_t i = 0; i < count; i++) {
        const char *b;
        int64_t l;
        forced[i] = ll_coerce_to_string_for_concat_sep(ll_force_for_print(elems[i]));
        as_string(forced[i], &b, &l, "concatStringsSep");
        ll_ctx_union_into(&ctx, ll_string_context_of(forced[i]));
        total += l;
        if (i + 1 < count) total += sep_len;
    }

    char *out = ll_arena_alloc((uint64_t)total);
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
    Value result = ll_make_string_value(out, (size_t)total);
    ll_string_attach_context(result, &ctx);
    return result;
}

static Value build_concatStringsSep(void) {
    return make_curried_native((void *)concatStringsSep2_impl, 2);
}

static Value addErrorContext2_impl(const Value *args, int64_t argc) {
    (void)argc;
    return args[1];
}
static Value build_addErrorContext(void) {
    return make_curried_native((void *)addErrorContext2_impl, 2);
}

static Value prim_isNull(const Value *args, int64_t argc) {
    (void)argc;
    return ll_make_imm(ll_tag_of(args[0]) == LL_TAG_IMM && ll_imm_subtag(args[0]) == LL_IMM_NULL
                               ? LL_IMM_TRUE
                               : LL_IMM_FALSE);
}
static Value prim_isString(const Value *args, int64_t argc) {
    (void)argc;
    /* Paths and strings share LL_TAG_PTR (see heap.h), and a Float is a
     * third kind that also shares this tag -- isString must exclude
     * both, or it wrongly returns true for a Path/Float argument. */
    return ll_make_imm(ll_tag_of(args[0]) == LL_TAG_PTR && !ll_value_is_path(args[0]) &&
                                   !ll_value_is_float(args[0])
                               ? LL_IMM_TRUE
                               : LL_IMM_FALSE);
}
static Value prim_isPath(const Value *args, int64_t argc) {
    (void)argc;
    return ll_make_imm(ll_tag_of(args[0]) == LL_TAG_PTR && ll_value_is_path(args[0])
                               ? LL_IMM_TRUE
                               : LL_IMM_FALSE);
}
static Value prim_isFloat(const Value *args, int64_t argc) {
    (void)argc;
    return ll_make_imm(ll_value_is_float(args[0]) ? LL_IMM_TRUE : LL_IMM_FALSE);
}
static Value prim_isList(const Value *args, int64_t argc) {
    (void)argc;
    return ll_make_imm(ll_tag_of(args[0]) == LL_TAG_LIST ? LL_IMM_TRUE : LL_IMM_FALSE);
}
static Value prim_isAttrs(const Value *args, int64_t argc) {
    (void)argc;
    return ll_make_imm(ll_tag_of(args[0]) == LL_TAG_ATTRS ? LL_IMM_TRUE : LL_IMM_FALSE);
}
static Value prim_isFunction(const Value *args, int64_t argc) {
    (void)argc;
    int tag = ll_tag_of(args[0]);
    return ll_make_imm(tag == LL_TAG_CLOSURE || tag == LL_TAG_PRIMOP ? LL_IMM_TRUE : LL_IMM_FALSE);
}

static Value prim_functionArgs(const Value *args, int64_t argc) {
    (void)argc;
    if (ll_tag_of(args[0]) != LL_TAG_CLOSURE) return ll_bshared_make_attrs(NULL, NULL, 0);
    const char *base = (const char *)(args[0].raw & ~0x7ULL);
    return *(const Value *)(base + LL_CLOSURE_OFF_FORMALS);
}
static Value prim_isInt(const Value *args, int64_t argc) {
    (void)argc;
    return ll_make_imm(ll_tag_of(args[0]) == LL_TAG_INT ? LL_IMM_TRUE : LL_IMM_FALSE);
}
static Value prim_isBool(const Value *args, int64_t argc) {
    (void)argc;
    if (ll_tag_of(args[0]) != LL_TAG_IMM) return ll_make_imm(LL_IMM_FALSE);
    int sub = ll_imm_subtag(args[0]);
    return ll_make_imm(sub == LL_IMM_TRUE || sub == LL_IMM_FALSE ? LL_IMM_TRUE : LL_IMM_FALSE);
}

static Value prim_floor(const Value *args, int64_t argc) {
    (void)argc;
    Value v = args[0];
    if (ll_tag_of(v) == LL_TAG_INT) return v;
    if (!ll_value_is_float(v)) fatal("floor: argument is not a number");
    double d = ll_float_value(v);
    int64_t t = (int64_t)d;
    if (d < 0 && (double)t != d) t -= 1;
    return ll_make_int(t);
}
static Value prim_ceil(const Value *args, int64_t argc) {
    (void)argc;
    Value v = args[0];
    if (ll_tag_of(v) == LL_TAG_INT) return v;
    if (!ll_value_is_float(v)) fatal("ceil: argument is not a number");
    double d = ll_float_value(v);
    int64_t t = (int64_t)d;
    if (d > 0 && (double)t != d) t += 1;
    return ll_make_int(t);
}

static Value prim_baseNameOf(const Value *args, int64_t argc) {
    (void)argc;
    int64_t len;
    const char *bytes = ll_ptr_bytes(args[0], &len);
    if (len > 0 && bytes[len - 1] == '/') len--; /* AT MOST ONE trailing slash stripped (an
                                                    * `if`, not a `while`) -- matches real
                                                    * Nix's legacyBaseNameOf exactly, not GNU
                                                    * basename's "any number". A prior `while`
                                                    * here silently stripped every trailing
                                                    * slash instead of just one -- do not
                                                    * change this back to a loop. */
    int64_t start = len;
    while (start > 0 && bytes[start - 1] != '/') start--;
    Value out = ll_make_string_value(bytes + start, (size_t)(len - start));
    /* real Nix's own `prim_baseNameOf` copies `args[0]`'s context onto
     * the result verbatim; `ll_string_context_of` on a Path is always
     * NULL, so this is a no-op for the Path-argument case. */
    const LlStringContext *ctx = ll_string_context_of(args[0]);
    if (!ll_ctx_is_empty(ctx)) ll_string_attach_context(out, ctx);
    return out;
}

static Value prim_dirOf(const Value *args, int64_t argc) {
    (void)argc;
    Value v = args[0];
    int64_t len;
    const char *bytes = ll_ptr_bytes(v, &len);
    int is_path = ll_value_is_path(v);
    /* asnix Paths are always already-resolved absolute paths with no
     * trailing slash (see compiler.c's AST_PATH/resolve_path_literal),
     * so "/" is the only case needing the real-Nix "isRoot, keep as-is"
     * special case -- everything else always contains a '/' to split on. */
    if (is_path) {
        if (len == 1 && bytes[0] == '/') return ll_make_path_value("/", 1);
        int64_t pos = len;
        while (pos > 0 && bytes[pos - 1] != '/') pos--;
        pos--;
        if (pos <= 0) return ll_make_path_value("/", 1);
        return ll_make_path_value(bytes, (size_t)pos);
    }
    int64_t pos = len;
    while (pos > 0 && bytes[pos - 1] != '/') pos--;
    /* a Path value never carries string context (context only applies
     * to strings-with-context), so only the String branches below need
     * to propagate it. */
    const LlStringContext *ctx = ll_string_context_of(v);
    if (pos == 0) {
        Value out = ll_make_string_value(".", 1);
        if (!ll_ctx_is_empty(ctx)) ll_string_attach_context(out, ctx);
        return out;
    }
    pos--;
    if (pos == 0) {
        Value out = ll_make_string_value("/", 1);
        if (!ll_ctx_is_empty(ctx)) ll_string_attach_context(out, ctx);
        return out;
    }
    Value out = ll_make_string_value(bytes, (size_t)pos);
    if (!ll_ctx_is_empty(ctx)) ll_string_attach_context(out, ctx);
    return out;
}

static Value build_baseNameOf(void) { return make_primop_value((void *)prim_baseNameOf, 1); }
static Value build_dirOf(void) { return make_primop_value((void *)prim_dirOf, 1); }
Value ll_make_baseNameOf_primop(void) { return make_primop_value((void *)prim_baseNameOf, 1); }
Value ll_make_dirOf_primop(void) { return make_primop_value((void *)prim_dirOf, 1); }

static Value prim_placeholder(const Value *args, int64_t argc) {
    (void)argc;
    LL_REQUIRE_STRING(args[0], "placeholder");
    int64_t name_len;
    const char *name_bytes;
    ll_bshared_string_bytes(args[0], &name_bytes, &name_len, "placeholder");

    size_t preimage_len = 11 + (size_t)name_len; /* "nix-output:" */
    char *preimage = malloc(preimage_len);
    memcpy(preimage, "nix-output:", 11);
    memcpy(preimage + 11, name_bytes, (size_t)name_len);

    uint8_t digest[LL_HASH_SHA256_LEN];
    int64_t rc = ll_hash_sha256(preimage, preimage_len, digest);
    free(preimage);
    if (rc != 0) {
        fprintf(stderr, "ll: placeholder: failed to load libcrypto (rc=%lld) -- enter the flake devShell\n",
                (long long)rc);
        exit(1);
    }

    size_t b32_len = ll_base32_encoded_length(LL_HASH_SHA256_LEN);
    char *out = malloc(1 + b32_len);
    out[0] = '/';
    ll_base32_encode(digest, LL_HASH_SHA256_LEN, out + 1);
    Value result = ll_make_string_value(out, 1 + b32_len);
    free(out);
    return result;
}
static Value build_placeholder(void) { return make_primop_value((void *)prim_placeholder, 1); }
Value ll_make_placeholder_primop(void) { return make_primop_value((void *)prim_placeholder, 1); }

static Value prim_toFile(const Value *args, int64_t argc) {
    (void)argc;
    LL_REQUIRE_STRING(args[0], "toFile");
    LL_REQUIRE_STRING(args[1], "toFile");

    char *name = ll_string_value_to_cstr(args[0]);
    ll_check_store_path_name(name);

    int64_t contents_len;
    const char *contents_bytes;
    ll_bshared_string_bytes(args[1], &contents_bytes, &contents_len, "toFile");

    /* `contents`' own context becomes this file's store references:
     * every Opaque context element is a plain-path reference; any other
     * kind (Built/DrvDeep -- i.e. `contents` mentions a derivation
     * output, not just a plain path) is a hard error, matching real
     * Nix's prim_toFile ("files created by builtins.toFile may not
     * reference derivations"). References, if any, get folded -- SORTED
     * ascending -- into the preimage's "type" field, which changes the
     * resulting store path's hash; this must match real Nix's own
     * makeType exactly, or a file whose contents reference another
     * toFile's output would hash differently than real Nix computes. */
    const LlStringContext *contents_ctx = ll_string_context_of(args[1]);
    if (contents_ctx && (contents_ctx->ndrvs > 0 || contents_ctx->ndeep > 0)) {
        fprintf(stderr,
                "ll: toFile: files created by builtins.toFile may not reference derivations, but '%s' does\n",
                name);
        exit(1);
    }
    size_t nrefs = contents_ctx ? contents_ctx->nsrcs : 0;
    char **refs = NULL;
    if (nrefs > 0) {
        refs = malloc(nrefs * sizeof(char *));
        memcpy(refs, contents_ctx->srcs, nrefs * sizeof(char *));
        /* Ascending lexicographic sort, matching real Nix's
         * StorePathSet (a std::set<StorePath> ordered by its own
         * basename string). */
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

    uint8_t inner_digest[LL_HASH_SHA256_LEN];
    int64_t rc = ll_hash_sha256(contents_bytes, (size_t)contents_len, inner_digest);
    if (rc != 0) {
        fprintf(stderr, "ll: toFile: failed to load libcrypto (rc=%lld) -- enter the flake devShell\n",
                (long long)rc);
        exit(1);
    }

    static const char hexdigits[] = "0123456789abcdef";
    char hashstr[7 + 64 + 1];
    memcpy(hashstr, "sha256:", 7);
    for (int i = 0; i < LL_HASH_SHA256_LEN; i++) {
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
    size_t n = ll_store_path_preimage(type_str, hashstr, name, preimage, sizeof(preimage));
    free(type_str);
    free(refs);
    if (n == 0) {
        fprintf(stderr, "ll: toFile: preimage too long\n");
        exit(1);
    }

    uint8_t outer_digest[LL_HASH_SHA256_LEN];
    rc = ll_hash_sha256(preimage, n, outer_digest);
    if (rc != 0) {
        fprintf(stderr, "ll: toFile: failed to load libcrypto (rc=%lld) -- enter the flake devShell\n",
                (long long)rc);
        exit(1);
    }

    char out_path[300];
    if (ll_make_store_path_from_digest(outer_digest, name, out_path, sizeof(out_path)) == 0) {
        fprintf(stderr, "ll: toFile: computed store path too long\n");
        exit(1);
    }
    Value out = ll_make_string_value(out_path, strlen(out_path));
    /* real Nix's own `toFile` attaches Opaque(storePath) -- ITS OWN
     * output path -- as the result string's context, NOT any context
     * collected from `contents` (that's folded into the type string
     * above instead). Without this, the resulting path reaches
     * `derivation {...}` as a plain opaque string with no trace of
     * being a store reference, so it never makes it into `inputSrcs`. */
    LlStringContext ctx;
    ll_ctx_init(&ctx);
    ll_ctx_add_opaque(&ctx, out_path);
    ll_string_attach_context(out, &ctx);
    return out;
}
static Value build_toFile(void) { return make_curried_native((void *)prim_toFile, 2); }

static Value build_isNull(void) { return make_primop_value((void *)prim_isNull, 1); }

Value ll_make_isNull_primop(void) { return make_primop_value((void *)prim_isNull, 1); }
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

static Value build_nixVersion(void) { return ll_make_string_value("2.35.2", 6); }

static Value build_currentSystem(void) { return ll_make_string_value("x86_64-linux", 12); }

static Value build_storeDir(void) { return ll_make_string_value("/nix/store", 10); }

static Value build_langVersion(void) { return ll_make_int(6); }

const LlBuiltinMiscDef LL_BUILTINS_MISC[] = {
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
    { "langVersion", build_langVersion },
    { "baseNameOf", build_baseNameOf },
    { "dirOf", build_dirOf },
    { "placeholder", build_placeholder },
    { "toFile", build_toFile },
};

const size_t LL_BUILTINS_MISC_COUNT = sizeof(LL_BUILTINS_MISC) / sizeof(LL_BUILTINS_MISC[0]);
