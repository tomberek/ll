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
#include "value_util.h"
#include "value_util.h"

static Value make_string(const char *s, size_t len) {
    return ll_make_string_value(s, len);
}

static Value prim_toString(const Value *args, int64_t argc) {
    (void)argc;
    return ll_coerce_to_string(args[0]);
}

static Value prim_abort(const Value *args, int64_t argc) {
    (void)argc;
    Value msg = ll_coerce_to_string(args[0]);
    int64_t len;
    const char *bytes = ll_ptr_bytes(msg, &len);
    fprintf(stderr, "ll: evaluation aborted with the following error message: '%.*s'\n", (int)len, bytes);
    exit(1);
}

static Value prim_throw(const Value *args, int64_t argc) {
    (void)argc;
    Value msg = ll_coerce_to_string(args[0]);
    int64_t len;
    const char *bytes = ll_ptr_bytes(msg, &len);
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

static Value hash_apply(const Value *args, int64_t (*hash_fn)(const void *, size_t, unsigned char *), size_t raw_len) {
    Value in = args[0];
    if (ll_tag_of(in) != LL_TAG_PTR || ll_value_is_path(in) || ll_value_is_float(in)) {
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

static Value prim_import(const Value *args, int64_t argc) {
    (void)argc;
    Value target = args[0];
    if (!ll_value_is_path(target) && ll_tag_of(target) != LL_TAG_PTR) {
        target = ll_coerce_to_string(target);
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
    { "hashString", prim_hashString, 1 }, /* true arity 2, curried */
    { "derivationStrict", ll_prim_derivation, 1 }, /* `derivation` itself is a wrapper, see below */
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
    { "fetchGit", ll_prim_fetchGit, 1 }, /* see builtins_fetchgit.c */
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

/* `skip_derivation_wrapper`: building the `derivation.nix` wrapper's OWN
 * `builtins` attrset must not recurse into building the wrapper again via
 * this same attrset's "derivation" entry -- 1 substitutes the plain
 * `derivationStrict` primop for "derivation" too in that one bootstrap
 * case. ll_make_builtins_attrs (below) is the ordinary
 * `skip_derivation_wrapper=0` case every other caller gets. */
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
    const LlBuiltinM54Def *const m54_tables[] = { LL_M54_BUILTINS };
    const size_t m54_counts[] = { LL_M54_BUILTINS_COUNT };
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
        /* Must be a 4-byte write -- entry's value bytes start right after sym (no pad),
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

Value ll_make_builtins_attrs_for_derivation_bootstrap(void) { return ll_make_builtins_attrs_ex(1); }
