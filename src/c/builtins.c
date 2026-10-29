/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "builtins.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ll/arena.h"
#include "ll/heap.h"
#include "builtins_attrs.h"
#include "builtins_attrs2.h"
#include "builtins_closure.h"
#include "builtins_compare.h"
#include "builtins_fetchgit.h"
#include "builtins_flakeref.h"
#include "builtins_json.h"
#include "builtins_list.h"
#include "builtins_list2.h"
#include "builtins_m23.h"
#include "builtins_m54.h"
#include "builtins_misc.h"
#include "builtins_path.h"
#include "builtins_regex.h"
#include "builtins_toml.h"
#include "builtins_tryeval.h"
#include "derivation.h"
#include "eval_file.h"
#include "hash_shim.h"
#include "symtab.h"
#include "tryeval.h"
#include "value_util.h" /* ll_coerce_to_string, M57e import __toString fallback */
#include "value_util.h"

static Value make_string(const char *s, size_t len) {
    return ll_make_string_value(s, len);
}

static Value prim_toString(const Value *args, int64_t argc) {
    (void)argc;
    return ll_coerce_to_string(args[0]);
}

/* `abort msg` -- unconditionally fatal (matches real Nix's core wording,
 * confirmed directly: "error: evaluation aborted with the following
 * error message: '<msg>'"; the "while calling" trace context real Nix
 * also prints isn't reproduced -- asnix has no call-trace infrastructure
 * at all, same simplification every other fatal error in this project
 * already makes). */
static Value prim_abort(const Value *args, int64_t argc) {
    (void)argc;
    Value msg = ll_coerce_to_string(args[0]);
    int64_t len;
    const char *bytes = ll_ptr_bytes(msg, &len);
    fprintf(stderr, "ll: evaluation aborted with the following error message: '%.*s'\n", (int)len, bytes);
    exit(1);
}

/* `throw msg` -- also unconditionally fatal, but with real Nix's distinct,
 * plainer wording (confirmed directly: `throw "x"` -> "error: x", not
 * `abort`'s "evaluation aborted with the following error message: 'x'").
 * Real Nix's `builtins.tryEval`/`try`-style catching would make these two
 * genuinely different (`throw` is catchable, `abort` never is) -- asnix
 * has no such mechanism, so the only observable difference here is the
 * message wording, a documented, narrower scope cut. One of real Nix's
 * bare root-scope globals too (found needed evaluating real nixpkgs,
 * `pkgs/top-level/default.nix` uses bare `throw`), see eval_file.c/
 * compiler.c's root scope. */
static Value prim_throw(const Value *args, int64_t argc) {
    (void)argc;
    Value msg = ll_coerce_to_string(args[0]);
    int64_t len;
    const char *bytes = ll_ptr_bytes(msg, &len);
    /* M30: routed through the catchable-throw mechanism instead of a
     * direct fprintf+exit -- `builtins.tryEval` must be able to catch
     * this (confirmed against real Nix: `throw` IS one of the two
     * things `tryEval` catches, see tryeval.h). Falls back to exactly
     * the same "ll: <msg>\n" + exit(1) behavior when no `tryEval` is
     * active, so top-level `throw` (the overwhelmingly common case)
     * looks identical to before. */
    char *formatted = ll_arena_alloc((size_t)len + 8);
    memcpy(formatted, "ll: ", 7);
    memcpy(formatted + 7, bytes, (size_t)len);
    formatted[7 + len] = '\n';
    ll_tryeval_raise_or_fatal(formatted, (size_t)(7 + len + 1));
}

static Value prim_typeOf(const Value *args, int64_t argc) {
    (void)argc;
    switch (ll_tag_of(args[0])) {
        case LL_TAG_INT: return make_string("int", 3);
        case LL_TAG_IMM:
            switch (ll_imm_subtag(args[0])) {
                case LL_IMM_TRUE:
                case LL_IMM_FALSE: return make_string("bool", 4);
                case LL_IMM_NULL: return make_string("null", 4);
                default: return make_string("imm", 3);
            }
        case LL_TAG_PTR:
            return ll_value_is_path(args[0]) ? make_string("path", 4)
                 : ll_value_is_float(args[0]) ? make_string("float", 5)
                                                  : make_string("string", 6);
        case LL_TAG_LIST: return make_string("list", 4);
        case LL_TAG_ATTRS: return make_string("set", 3);
        case LL_TAG_CLOSURE:
        case LL_TAG_PRIMOP: return make_string("lambda", 6);
        default: return make_string("unknown", 7);
    }
}

static Value prim_length(const Value *args, int64_t argc) {
    (void)argc;
    if (ll_tag_of(args[0]) != LL_TAG_LIST) {
        fprintf(stderr, "ll: length: argument is not a list\n");
        exit(1);
    }
    const char *base = (const char *)(args[0].raw & ~0x7ULL);
    int64_t count = *(const int64_t *)(base + LL_LIST_OFF_COUNT);
    return ll_make_int(count);
}

static Value make_primop_value(void *fn, int64_t arity) {
    void *mem = ll_arena_alloc(LL_PRIMOP_SIZE);
    *(void **)((char *)mem + LL_PRIMOP_OFF_FN) = fn;
    *(int64_t *)((char *)mem + LL_PRIMOP_OFF_ARITY) = arity;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | LL_TAG_PRIMOP };
}

/*
 * builtins.hashString algo s -- M8. Arity in real Nix is 2, but that can't
 * be registered directly as a single arity-2 BuiltinDef here: parse_app
 * (parser.c) always desugars multi-argument application into nested
 * single-argument AST_APP nodes (`f x y` => App(App(f,x),y), exactly like
 * real Nix's own curried-application grammar), and compiler.c's AST_APP
 * case always emits `ib_call(b, 1)` -- so `builtins.hashString "sha256"
 * "hello"` reaches OP_CALL as *two* separate argc=1 calls, not one argc=2
 * call. OP_CALL's primop branch (op_call.S) requires an exact arity match
 * per call and hands the native fn nothing but `(args, argc)` -- no
 * self/closure pointer, so there is no channel to carry data (like which
 * algorithm was picked) from a first call into a second one (see
 * docs/abi.md's "Primop calls (M7)" section: "a plain C function, nothing
 * asnix-specific about its own convention").
 *
 * The fix mirrors how curried *user* lambdas already get multi-arg calls
 * to work in this codebase: `a: b: body` is two nested arity-1 closures,
 * where calling the outer one returns a new callable (the inner closure)
 * that already has `a` baked into its captured env. `prim_hashString`
 * does the native-code equivalent: it's itself arity 1, takes just the
 * algorithm name, and -- since there are only 4 supported algorithms,
 * known statically -- returns one of 4 dedicated arity-1 PrimOps
 * (`prim_hashString_apply_{md5,sha1,sha256,sha512}`), each with the
 * chosen algorithm already baked into *which function pointer it is*
 * rather than any captured runtime data. No mutable global, no
 * extra-data-after-the-PrimOp-struct trick, no asm changes needed.
 */
static Value hash_apply(const Value *args, int64_t (*hash_fn)(const void *, size_t, unsigned char *), size_t raw_len) {
    Value in = args[0];
    if (ll_tag_of(in) != LL_TAG_PTR || ll_value_is_path(in) || ll_value_is_float(in)) {
        /* Real Nix is strict here too: `hashString "sha256" ./foo.nix`
         * fails with "expected a string but found a path" (checked
         * against nix-instantiate) -- a path isn't auto-coerced. */
        fprintf(stderr, "ll: hashString: second argument must be a string\n");
        exit(1);
    }
    int64_t len;
    const char *bytes = ll_ptr_bytes(in, &len);

    unsigned char raw[LL_HASH_SHA512_LEN]; /* largest digest; raw_len is always <= this */
    int64_t rc = hash_fn(bytes, (size_t)len, raw);
    if (rc != 0) {
        fprintf(stderr, "ll: hashString: failed to load libcrypto (rc=%lld) -- "
                         "enter the flake devShell (needs LL_LIBCRYPTO_LIB / openssl)\n",
                (long long)rc);
        exit(1);
    }

    char hex[2 * LL_HASH_SHA512_LEN];
    ll_hash_to_hex(raw, raw_len, hex);
    return make_string(hex, 2 * raw_len);
}

static Value prim_hashString_apply_md5(const Value *args, int64_t argc) {
    (void)argc;
    return hash_apply(args, ll_hash_md5, LL_HASH_MD5_LEN);
}
static Value prim_hashString_apply_sha1(const Value *args, int64_t argc) {
    (void)argc;
    return hash_apply(args, ll_hash_sha1, LL_HASH_SHA1_LEN);
}
static Value prim_hashString_apply_sha256(const Value *args, int64_t argc) {
    (void)argc;
    return hash_apply(args, ll_hash_sha256, LL_HASH_SHA256_LEN);
}
static Value prim_hashString_apply_sha512(const Value *args, int64_t argc) {
    (void)argc;
    return hash_apply(args, ll_hash_sha512, LL_HASH_SHA512_LEN);
}

static Value prim_hashString(const Value *args, int64_t argc) {
    (void)argc;
    Value algo = args[0];
    if (ll_tag_of(algo) != LL_TAG_PTR || ll_value_is_path(algo) || ll_value_is_float(algo)) {
        fprintf(stderr, "ll: hashString: first argument (algorithm name) must be a string\n");
        exit(1);
    }
    int64_t len;
    const char *name = ll_ptr_bytes(algo, &len);

    if (len == 3 && memcmp(name, "md5", 3) == 0) return make_primop_value((void *)prim_hashString_apply_md5, 1);
    if (len == 4 && memcmp(name, "sha1", 4) == 0) return make_primop_value((void *)prim_hashString_apply_sha1, 1);
    if (len == 6 && memcmp(name, "sha256", 6) == 0) return make_primop_value((void *)prim_hashString_apply_sha256, 1);
    if (len == 6 && memcmp(name, "sha512", 6) == 0) return make_primop_value((void *)prim_hashString_apply_sha512, 1);

    fprintf(stderr, "ll: hashString: unsupported algorithm \"%.*s\" (expected md5/sha1/sha256/sha512)\n",
            (int)len, name);
    exit(1);
}

/*
 * builtins.import target -- runs the file at `target` (a Path, or an
 * absolute-path String) through the full lex/parse/compile/eval pipeline
 * via ll_eval_file() (src/c/eval_file.c) and returns its Value. Not
 * exposed as a bare `import` identifier (only `builtins.import`): this
 * codebase doesn't give user programs an implicit "with builtins" root
 * scope the way real Nix does for `import`/`map`/`throw`/etc -- see
 * docs/architecture.md's import/path scope notes. A relative-path String
 * (e.g. `import "foo.nix"`) is rejected exactly like real Nix does
 * (checked against nix-instantiate: "string ... doesn't represent an
 * absolute path") rather than silently resolving it against cwd --
 * there's no "current file" to resolve a bare string against here the
 * way a path *literal* resolves against its own file at compile time.
 *
 * M57e: before rejecting an argument that's neither a path nor a string,
 * try the `__toString`/`outPath` coercion protocol (ll_coerce_to_string,
 * value_util.c) first -- real Nix's `import` goes through the same
 * general `coerceToString` every other path/string consumer does
 * (`eval.cc`), so `import { __toString = _: toString ./foo.nix; }` is
 * valid (confirmed against real Nix, which tries `__toString` even
 * before `outPath` -- same order ll_coerce_to_string already uses for
 * every other caller).
 */
static Value prim_import(const Value *args, int64_t argc) {
    (void)argc;
    Value target = args[0];
    if (!ll_value_is_path(target) && ll_tag_of(target) != LL_TAG_PTR) {
        target = ll_coerce_to_string(target); /* fatal itself if this also fails */
    }
    int64_t len;
    const char *bytes;
    if (ll_value_is_path(target)) {
        bytes = ll_ptr_bytes(target, &len); /* already absolute -- see compiler.c's AST_PATH case */
    } else if (ll_tag_of(target) == LL_TAG_PTR) {
        bytes = ll_ptr_bytes(target, &len);
        if (len == 0 || bytes[0] != '/') {
            fprintf(stderr, "ll: import: string '%.*s' doesn't represent an absolute path\n", (int)len, bytes);
            exit(1);
        }
    } else {
        fprintf(stderr, "ll: import: argument must be a path or an absolute-path string\n");
        exit(1);
    }
    char *path = ll_arena_alloc((size_t)len + 1);
    memcpy(path, bytes, (size_t)len);
    path[len] = '\0';
    return ll_eval_file(path);
}

typedef Value (*PrimFn)(const Value *, int64_t);

typedef struct {
    const char *name;
    PrimFn fn;
    int64_t arity;
} BuiltinDef;

static const BuiltinDef BUILTINS[] = {
    { "toString", prim_toString, 1 },
    { "typeOf", prim_typeOf, 1 },
    { "length", prim_length, 1 },
    { "hashString", prim_hashString, 1 }, /* true arity 2, curried -- see hash_apply's comment above */
    { "derivationStrict", ll_prim_derivation, 1 }, /* M58: `derivation` itself is a wrapper, see below */
    { "import", prim_import, 1 },
    { "abort", prim_abort, 1 },
    { "throw", prim_throw, 1 },
    { "pathExists", ll_prim_pathExists, 1 },
    { "readFile", ll_prim_readFile, 1 },
    { "readFileType", ll_prim_readFileType, 1 },
    { "readDir", ll_prim_readDir, 1 },
    { "toPath", ll_prim_toPath, 1 },
    { "getEnv", ll_prim_getEnv, 1 },
    { "parseDrvName", ll_prim_parseDrvName, 1 },
    { "fetchGit", ll_prim_fetchGit, 1 }, /* M82: see builtins_fetchgit.c */
};

Value ll_make_import_primop(void) {
    return make_primop_value((void *)prim_import, 1);
}

Value ll_make_abort_primop(void) {
    return make_primop_value((void *)prim_abort, 1);
}

Value ll_make_throw_primop(void) {
    return make_primop_value((void *)prim_throw, 1);
}

Value ll_make_toString_primop(void) {
    return make_primop_value((void *)prim_toString, 1);
}

/* M58: `derivation` moved to derivation.c's ll_make_derivation_wrapper_value
 * (a Nix-level wrapper around this same primop, now bound as
 * `derivationStrict` instead) -- see derivation.h's header comment. */

/* Merges this file's own BUILTINS[] with the builtins-expansion modules
 * added across three passes (M17: list/attrs/misc; M19: closure/list2/
 * attrs2/compare; this pass: regex), each kept in its own translation
 * unit so agents working in parallel (M17) or on independent feature
 * batches (M19, this pass) don't textually collide on one shared array
 * -- see builtins_list.h/builtins_attrs.h/builtins_misc.h/
 * builtins_closure.h/builtins_list2.h/builtins_attrs2.h/
 * builtins_compare.h/builtins_regex.h's own "integration step" comments,
 * which this function fulfills.
 *
 * Two source shapes, merged via two small adapter loops: the plain
 * {name,fn,arity} shape (this file's own BUILTINS[], plus
 * ll_builtins_list/LL_ATTRS_BUILTINS -- arity>1 entries here are
 * themselves arity-1 PrimOps that *return* the next curried stage, see
 * those modules' own comments) is registered as a single PrimOp of the
 * given arity; the {name,build()} shape (LL_BUILTINS_MISC and this
 * pass's four new modules -- build() already returns a fully-formed
 * Value, since several entries are curried Closure chains rather than a
 * single PrimOp, see builtins_shared.h's make_curried_native) is
 * registered by just calling build(). */
/* `skip_derivation_wrapper`: see eval_file.c's make_root_env_ex comment
 * -- building the `derivation.nix` wrapper's OWN `builtins` attrset
 * must not recurse into building the wrapper again via this same
 * attrset's "derivation" entry. 1 substitutes the plain
 * `derivationStrict` primop for "derivation" too in that one bootstrap
 * case (harmless: the wrapper source only ever uses bare `derivationStrict`,
 * never `builtins.derivation`). ll_make_builtins_attrs (below) is the
 * ordinary `skip_derivation_wrapper=0` case every other caller gets. */
static Value ll_make_builtins_attrs_ex(int skip_derivation_wrapper) {
    static const BuiltinDef *const plain_tables[] = { BUILTINS };
    static const size_t plain_counts[] = { sizeof(BUILTINS) / sizeof(BUILTINS[0]) };
    const LlBuiltinDef *const list_shaped_tables[] = { ll_builtins_list, LL_ATTRS_BUILTINS };
    const size_t list_shaped_counts[] = { ll_builtins_list_count, LL_ATTRS_BUILTINS_COUNT };
    const LlBuiltinMiscDef *const build_tables[] = {
        LL_BUILTINS_MISC, LL_CLOSURE_BUILTINS, LL_LIST2_BUILTINS, LL_ATTRS2_BUILTINS, LL_COMPARE_BUILTINS,
        LL_M23_BUILTINS, LL_REGEX_BUILTINS, LL_TOML_BUILTINS, LL_TRYEVAL_BUILTINS, LL_JSON_BUILTINS,
        LL_DERIVATION_BUILTINS, LL_PATH_BUILTINS,
    };
    const size_t build_counts[] = {
        LL_BUILTINS_MISC_COUNT, LL_CLOSURE_BUILTINS_COUNT, LL_LIST2_BUILTINS_COUNT,
        LL_ATTRS2_BUILTINS_COUNT, LL_COMPARE_BUILTINS_COUNT, LL_M23_BUILTINS_COUNT, LL_REGEX_BUILTINS_COUNT,
        LL_TOML_BUILTINS_COUNT, LL_TRYEVAL_BUILTINS_COUNT, LL_JSON_BUILTINS_COUNT,
        LL_DERIVATION_BUILTINS_COUNT, LL_PATH_BUILTINS_COUNT,
    };
    /* M54: a further builtins-expansion pass (bitAnd/bitOr/bitXor/trace/
     * zipAttrsWith/toXML/convertHash/getContext/appendContext/
     * scopedImport) -- same {name,build()} shape, kept as its own table
     * (builtins_m54.h) rather than threaded through the arrays above, so
     * it's visually obvious which pass added what. */
    const LlBuiltinM54Def *const m54_tables[] = { LL_M54_BUILTINS };
    const size_t m54_counts[] = { LL_M54_BUILTINS_COUNT };
    /* M66: builtins.parseFlakeRef/flakeRefToString -- same {name,build()}
     * shape, own table for the same "separate translation unit, separate
     * agent" reason M54's table is split out (builtins_flakeref.h). */
    const LlBuiltinFlakerefDef *const flakeref_tables[] = { LL_FLAKEREF_BUILTINS };
    const size_t flakeref_counts[] = { LL_FLAKEREF_BUILTINS_COUNT };

    size_t n = 0;
    for (size_t t = 0; t < sizeof(plain_counts) / sizeof(plain_counts[0]); t++) n += plain_counts[t];
    for (size_t t = 0; t < sizeof(list_shaped_counts) / sizeof(list_shaped_counts[0]); t++) n += list_shaped_counts[t];
    for (size_t t = 0; t < sizeof(build_counts) / sizeof(build_counts[0]); t++) n += build_counts[t];
    for (size_t t = 0; t < sizeof(m54_counts) / sizeof(m54_counts[0]); t++) n += m54_counts[t];
    for (size_t t = 0; t < sizeof(flakeref_counts) / sizeof(flakeref_counts[0]); t++) n += flakeref_counts[t];

    uint32_t *syms = malloc(n * sizeof(uint32_t));
    Value *vals = malloc(n * sizeof(Value));
    size_t *order = malloc(n * sizeof(size_t));

    size_t k = 0;
    for (size_t t = 0; t < sizeof(plain_counts) / sizeof(plain_counts[0]); t++) {
        for (size_t i = 0; i < plain_counts[t]; i++, k++) {
            syms[k] = ll_intern(plain_tables[t][i].name, strlen(plain_tables[t][i].name));
            vals[k] = make_primop_value((void *)plain_tables[t][i].fn, plain_tables[t][i].arity);
        }
    }
    for (size_t t = 0; t < sizeof(list_shaped_counts) / sizeof(list_shaped_counts[0]); t++) {
        for (size_t i = 0; i < list_shaped_counts[t]; i++, k++) {
            syms[k] = ll_intern(list_shaped_tables[t][i].name, strlen(list_shaped_tables[t][i].name));
            vals[k] = make_primop_value((void *)list_shaped_tables[t][i].fn, list_shaped_tables[t][i].arity);
        }
    }
    for (size_t t = 0; t < sizeof(build_counts) / sizeof(build_counts[0]); t++) {
        for (size_t i = 0; i < build_counts[t]; i++, k++) {
            LlBuiltinMiscBuilder build = build_tables[t][i].build;
            if (skip_derivation_wrapper && build == ll_make_derivation_wrapper_value) {
                build = ll_make_derivation_strict_primop;
            }
            syms[k] = ll_intern(build_tables[t][i].name, strlen(build_tables[t][i].name));
            vals[k] = build();
        }
    }
    for (size_t t = 0; t < sizeof(m54_counts) / sizeof(m54_counts[0]); t++) {
        for (size_t i = 0; i < m54_counts[t]; i++, k++) {
            syms[k] = ll_intern(m54_tables[t][i].name, strlen(m54_tables[t][i].name));
            vals[k] = m54_tables[t][i].build();
        }
    }
    for (size_t t = 0; t < sizeof(flakeref_counts) / sizeof(flakeref_counts[0]); t++) {
        for (size_t i = 0; i < flakeref_counts[t]; i++, k++) {
            syms[k] = ll_intern(flakeref_tables[t][i].name, strlen(flakeref_tables[t][i].name));
            vals[k] = flakeref_tables[t][i].build();
        }
    }

    for (size_t i = 0; i < n; i++) order[i] = i;
    for (size_t i = 1; i < n; i++) {
        size_t j = i;
        while (j > 0 && syms[order[j - 1]] > syms[order[j]]) {
            size_t tmp = order[j - 1];
            order[j - 1] = order[j];
            order[j] = tmp;
            j--;
        }
    }

    void *mem = ll_arena_alloc(8 + n * LL_ATTR_SIZE);
    *(int64_t *)((char *)mem + LL_ATTRS_OFF_COUNT) = (int64_t)n;
    for (size_t i = 0; i < n; i++) {
        char *entry = (char *)mem + LL_ATTRS_OFF_ENTRIES + i * LL_ATTR_SIZE;
        /* M103: must be a 4-byte write -- entry's value bytes now start right after sym (no pad),
         * so an 8-byte write here would clobber them. */
        *(uint32_t *)(entry + LL_ATTR_OFF_SYM) = syms[order[i]];
        *(Value *)(entry + LL_ATTR_OFF_VALUE) = vals[order[i]];
    }
    free(syms);
    free(vals);
    free(order);
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | LL_TAG_ATTRS };
}

Value ll_make_builtins_attrs(void) { return ll_make_builtins_attrs_ex(0); }

/* M58: the `builtins` attrset half of eval_file.c's
 * ll_eval_file_for_derivation_bootstrap cycle-break -- see
 * ll_make_builtins_attrs_ex's comment above. */
Value ll_make_builtins_attrs_for_derivation_bootstrap(void) { return ll_make_builtins_attrs_ex(1); }
