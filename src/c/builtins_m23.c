/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#define _DEFAULT_SOURCE /* lstat() is POSIX, not C11 */
#include "builtins_m23.h"

#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "ll/arena.h"
#include "ll/heap.h"
#include "ll/ir.h"
#include "builtins_shared.h"
#include "hash_shim.h"
#include "symtab.h"
#include "value_util.h"

/* `seq e1 e2` forces e1 (shallow only) then returns e2 (also only
 * shallow-forced), matching real Nix's `prim_seq`. `deepSeq e1 e2`
 * instead walks e1 recursively into every list element / attrset value,
 * matching real Nix's `forceValueDeep`, but still only shallow-forces
 * e2.
 *
 * deepSeq's recursion needs cycle detection: a self-referential value
 * (`let as = { x = 123; y = as; }; in as`) is legal, lazy Nix and must
 * not infinite-loop just because `deepSeq` walks it. Do not remove the
 * `seen` set below -- without it, a self-referential deepSeq hangs.
 * Dedups by the forced heap-object pointer identity (Attrs/List
 * address) rather than real Nix's by-Value-slot-address scheme. */
typedef struct {
    const void **items;
    size_t len, cap;
} PtrSet;

static int ptrset_contains_or_add(PtrSet *s, const void *p) {
    for (size_t i = 0; i < s->len; i++) {
        if (s->items[i] == p) return 1;
    }
    if (s->len == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->items = realloc(s->items, s->cap * sizeof(const void *));
    }
    s->items[s->len++] = p;
    return 0;
}

static void deep_force_rec(Value v, PtrSet *seen) {
    v = ll_force_for_print(v);
    int tag = ll_tag_of(v);
    if (tag != LL_TAG_ATTRS && tag != LL_TAG_LIST) return;
    const void *ptr = (const void *)(uintptr_t)(v.raw & ~(uint64_t)LL_TAG_MASK);
    if (ptrset_contains_or_add(seen, ptr)) return; /* already visited this object -- a cycle */
    if (tag == LL_TAG_ATTRS) {
        int64_t n = ll_bshared_attrs_count(v);
        for (int64_t i = 0; i < n; i++) deep_force_rec(ll_bshared_attrs_value(v, i), seen);
    } else {
        int64_t n = ll_bshared_list_count(v);
        const Value *elems = ll_bshared_list_elems(v);
        for (int64_t i = 0; i < n; i++) deep_force_rec(elems[i], seen);
    }
}

static Value seq2_impl(const Value *args, int64_t argc) {
    (void)argc;
    ll_force_for_print(args[0]);
    return ll_force_for_print(args[1]);
}
static Value build_seq(void) { return ll_bshared_make_curried_native((void *)seq2_impl, 2); }

static Value deepSeq2_impl(const Value *args, int64_t argc) {
    (void)argc;
    PtrSet seen = { 0 };
    deep_force_rec(args[0], &seen);
    free(seen.items);
    return ll_force_for_print(args[1]);
}
static Value build_deepSeq(void) { return ll_bshared_make_curried_native((void *)deepSeq2_impl, 2); }

/* Filesystem path resolution shared by pathExists/readFile/readFileType/
 * readDir/toPath: accepts a Path, an absolute-path String, or anything
 * ll_coerce_to_string's __toString/outPath protocol can turn into one --
 * fatal on anything else or a non-absolute string. Returns a
 * NUL-terminated, arena-owned C string. */
static char *resolve_fs_path(Value v, const char *who) {
    Value coerced = ll_coerce_to_string(v); /* fatal on anything uncoercible */
    int64_t len;
    const char *bytes = ll_ptr_bytes(coerced, &len);
    if (len == 0 || bytes[0] != '/') {
        fprintf(stderr, "ll: %s: string '%.*s' doesn't represent an absolute path\n", who, (int)len, bytes);
        exit(1);
    }
    char *path = ll_arena_alloc((size_t)len + 1);
    memcpy(path, bytes, (size_t)len);
    path[len] = '\0';
    return path;
}

/* `builtins.pathExists p` -- real Nix's one wrinkle: when `p` is a
 * String ending in "/" or "/." it must resolve to a DIRECTORY
 * specifically (not just exist), using a symlink-following `stat`;
 * otherwise plain `lstat` (so a *broken* symlink still counts as
 * "exists", matching real Nix). Never fatal on a missing path -- only
 * on an uncoercible/non-absolute argument. */
static Value prim_pathExists(const Value *args, int64_t argc) {
    (void)argc;
    Value v = ll_force_for_print(args[0]);
    int must_be_dir = 0;
    if (ll_tag_of(v) == LL_TAG_PTR && !ll_value_is_path(v) && !ll_value_is_float(v)) {
        int64_t slen;
        const char *s = ll_ptr_bytes(v, &slen);
        if (slen >= 1 && s[slen - 1] == '/') must_be_dir = 1;
        else if (slen >= 2 && s[slen - 2] == '/' && s[slen - 1] == '.') must_be_dir = 1;
    }
    char *path = resolve_fs_path(v, "pathExists");
    struct stat st;
    int exists;
    if (must_be_dir) {
        exists = (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
    } else {
        exists = (lstat(path, &st) == 0);
    }
    return ll_make_imm(exists ? LL_IMM_TRUE : LL_IMM_FALSE);
}

/* Shared by readFile/hashFile: reads the whole file at `path` into a
 * freshly malloc'd buffer (caller frees), fatal on any failure --
 * same ftell()==-1-on-a-directory defensive check eval_file.c's own
 * read_file() already established (a directory "succeeds" opening via
 * fopen on Linux but can't seek). */
static char *read_whole_file(const char *path, const char *who, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "ll: %s: cannot open '%s'\n", who, path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) {
        fprintf(stderr, "ll: %s: cannot read '%s' (not a regular file?)\n", who, path);
        exit(1);
    }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)size);
    size_t n = size > 0 ? fread(buf, 1, (size_t)size, f) : 0;
    fclose(f);
    *out_len = n;
    return buf;
}

/* `builtins.readFile p` -- fatal if the file can't be opened/read
 * (matching real Nix's hard error on a missing file, unlike
 * pathExists). */
static Value prim_readFile(const Value *args, int64_t argc) {
    (void)argc;
    char *path = resolve_fs_path(args[0], "readFile");
    size_t len;
    char *buf = read_whole_file(path, "readFile", &len);
    Value result = ll_make_string_value(buf, len);
    free(buf);
    return result;
}

static const char *stat_type_str(const struct stat *st) {
    if (S_ISDIR(st->st_mode)) return "directory";
    if (S_ISREG(st->st_mode)) return "regular";
    if (S_ISLNK(st->st_mode)) return "symlink";
    return "unknown";
}

/* `builtins.readFileType p` -- uses `lstat` (does NOT follow the final
 * symlink component -- a symlink reports "symlink" regardless of its
 * target's type). */
static Value prim_readFileType(const Value *args, int64_t argc) {
    (void)argc;
    char *path = resolve_fs_path(args[0], "readFileType");
    struct stat st;
    if (lstat(path, &st) != 0) {
        fprintf(stderr, "ll: readFileType: cannot stat '%s'\n", path);
        exit(1);
    }
    const char *kind = stat_type_str(&st);
    return ll_make_string_value(kind, strlen(kind));
}

/* `builtins.readDir p` -- an attrset of directory-entry-name -> file-type
 * string (same four strings as readFileType, same lstat-not-following
 * convention for each entry). `.`/`..` are skipped, matching real Nix.
 *
 * `readdir` already returns `d_type` for free (populated straight from
 * the `getdents64` buffer on any filesystem that supports it) -- trust
 * it when it's not DT_UNKNOWN: DT_DIR/DT_REG/DT_LNK map directly to the
 * same three non-"unknown" strings stat_type_str would've produced from
 * an lstat of the same entry, so this is not a behavior change, just
 * skipping a redundant syscall. Only fall back to the lstat path when
 * d_type genuinely can't tell us (DT_UNKNOWN). */
static Value prim_readDir(const Value *args, int64_t argc) {
    (void)argc;
    char *path = resolve_fs_path(args[0], "readDir");
    DIR *dir = opendir(path);
    if (!dir) {
        fprintf(stderr, "ll: readDir: cannot open directory '%s'\n", path);
        exit(1);
    }
    uint32_t *syms = NULL;
    Value *vals = NULL;
    size_t n = 0, cap = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        size_t namelen = strlen(ent->d_name);
        const char *kind;
        switch (ent->d_type) {
            case DT_DIR: kind = "directory"; break;
            case DT_REG: kind = "regular"; break;
            case DT_LNK: kind = "symlink"; break;
            case DT_UNKNOWN: {
                char *entpath = malloc(strlen(path) + 1 + namelen + 1);
                snprintf(entpath, strlen(path) + 1 + namelen + 1, "%s/%s", path, ent->d_name);
                struct stat st;
                kind = (lstat(entpath, &st) == 0) ? stat_type_str(&st) : "unknown";
                free(entpath);
                break;
            }
            default: kind = "unknown"; break; /* DT_BLK/DT_CHR/DT_FIFO/DT_SOCK */
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            syms = realloc(syms, cap * sizeof(uint32_t));
            vals = realloc(vals, cap * sizeof(Value));
        }
        syms[n] = ll_intern(ent->d_name, namelen);
        vals[n] = ll_make_string_value(kind, strlen(kind));
        n++;
    }
    closedir(dir);
    Value result = ll_bshared_make_attrs(syms, vals, (int64_t)n);
    free(syms);
    free(vals);
    return result;
}

/* `builtins.toPath p` -- DEPRECATED even in real Nix: despite the name,
 * it returns a plain STRING (not a Path value), just the resolved
 * absolute-path text. */
static Value prim_toPath(const Value *args, int64_t argc) {
    (void)argc;
    Value coerced = ll_coerce_to_string(args[0]);
    int64_t len;
    const char *bytes = ll_ptr_bytes(coerced, &len);
    if (len == 0 || bytes[0] != '/') {
        fprintf(stderr, "ll: string '%.*s' doesn't represent an absolute path\n", (int)len, bytes);
        exit(1);
    }
    return ll_make_string_value(bytes, (size_t)len);
}

/* `builtins.getEnv name` -- real Nix gates this behind pure-eval mode
 * (erroring instead of reading the real environment); asnix has no such
 * mode, so this always reads the real environment, matching the
 * `--impure`/default-mode behavior. Empty string if unset, matching
 * real Nix either way. */
static Value prim_getEnv(const Value *args, int64_t argc) {
    (void)argc;
    LL_REQUIRE_STRING(args[0], "getEnv");
    int64_t len;
    const char *bytes = ll_ptr_bytes(args[0], &len);
    char *name = ll_arena_alloc((size_t)len + 1);
    memcpy(name, bytes, (size_t)len);
    name[len] = '\0';
    const char *val = getenv(name);
    if (!val) return ll_make_string_value("", 0);
    return ll_make_string_value(val, strlen(val));
}

/* `builtins.hashFile algo path` -- reads the whole file then hashes it,
 * same four algorithms and hex encoding as `builtins.hashString`. */
static Value hashFile_apply(const char *path, int64_t (*hash_fn)(const void *, size_t, unsigned char *),
                             size_t raw_len) {
    size_t len;
    char *buf = read_whole_file(path, "hashFile", &len);
    unsigned char raw[LL_HASH_SHA512_LEN];
    int64_t rc = hash_fn(buf, len, raw);
    free(buf);
    if (rc != 0) {
        fprintf(stderr, "ll: hashFile: failed to load libcrypto (rc=%lld) -- "
                         "enter the flake devShell (needs LL_LIBCRYPTO_LIB / openssl)\n",
                (long long)rc);
        exit(1);
    }
    char hex[2 * LL_HASH_SHA512_LEN];
    ll_hash_to_hex(raw, raw_len, hex);
    return ll_make_string_value(hex, 2 * raw_len);
}

static Value hashFile2_impl(const Value *args, int64_t argc) {
    (void)argc;
    LL_REQUIRE_STRING(args[0], "hashFile");
    int64_t len;
    const char *name = ll_ptr_bytes(args[0], &len);
    char *path = resolve_fs_path(args[1], "hashFile");

    if (len == 3 && memcmp(name, "md5", 3) == 0) return hashFile_apply(path, ll_hash_md5, LL_HASH_MD5_LEN);
    if (len == 4 && memcmp(name, "sha1", 4) == 0) return hashFile_apply(path, ll_hash_sha1, LL_HASH_SHA1_LEN);
    if (len == 6 && memcmp(name, "sha256", 6) == 0)
        return hashFile_apply(path, ll_hash_sha256, LL_HASH_SHA256_LEN);
    if (len == 6 && memcmp(name, "sha512", 6) == 0)
        return hashFile_apply(path, ll_hash_sha512, LL_HASH_SHA512_LEN);
    fprintf(stderr, "ll: hashFile: unsupported algorithm \"%.*s\" (expected md5/sha1/sha256/sha512)\n", (int)len,
            name);
    exit(1);
}
static Value build_hashFile(void) { return ll_bshared_make_curried_native((void *)hashFile2_impl, 2); }

/* intersectAttrs e1 e2 -- the subset of e2 whose NAMES also appear in
 * e1, keeping e2's (still-lazy) values. e1's values must never be
 * forced -- only its attribute *names* are consulted (nixpkgs's
 * `callPackageWith autoArgs f args` relies on this, passing `abort`-
 * valued formal-argument placeholders through as e1).
 *
 * Walks whichever side is smaller and binary-searches into the other,
 * for O(min(n1,n2) log max(n1,n2)) -- correct regardless of which side
 * is walked, since both inputs are sorted by symbol id (so the output
 * comes out already sorted, no re-sort needed) and the emitted value
 * always comes from e2 either way. */
Value ll_prim_intersectAttrs2_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value e1 = args[0], e2 = args[1];
    LL_REQUIRE_ATTRS(e1, "intersectAttrs");
    LL_REQUIRE_ATTRS(e2, "intersectAttrs");
    int64_t n1 = ll_bshared_attrs_count(e1);
    int64_t n2 = ll_bshared_attrs_count(e2);
    int64_t cap = n1 < n2 ? n1 : n2;
    uint32_t *syms = cap > 0 ? malloc((size_t)cap * sizeof(uint32_t)) : NULL;
    Value *vals = cap > 0 ? malloc((size_t)cap * sizeof(Value)) : NULL;
    int64_t out_n = 0;
    if (n1 <= n2) {
        for (int64_t i = 0; i < n1; i++) {
            uint32_t sym = ll_bshared_attrs_sym(e1, i);
            int64_t j = ll_bshared_attrs_find(e2, sym);
            if (j >= 0) {
                syms[out_n] = sym;
                vals[out_n] = ll_bshared_attrs_value(e2, j);
                out_n++;
            }
        }
    } else {
        for (int64_t i = 0; i < n2; i++) {
            uint32_t sym = ll_bshared_attrs_sym(e2, i);
            if (ll_bshared_attrs_find(e1, sym) >= 0) {
                syms[out_n] = sym;
                vals[out_n] = ll_bshared_attrs_value(e2, i);
                out_n++;
            }
        }
    }
    Value result = ll_bshared_make_attrs(syms, vals, out_n); /* already sorted -- re-sort is a cheap no-op */
    free(syms);
    free(vals);
    return result;
}
static Value build_intersectAttrs(void) {
    return ll_bshared_make_curried_native((void *)ll_prim_intersectAttrs2_impl, 2);
}

/* groupBy f list -- buckets `list`'s elements by `f elem` (which must
 * force to a string), preserving each bucket's original relative
 * order.
 *
 * Below LL_GROUPBY_HASH_MIN, a plain linear bucket-search is used.
 * Above it, an open-addressing hash index (keyed on the already-
 * interned `sym`, sized once from `n`) gives O(1)-average lookup
 * instead of an O(n * distinct-keys) scan. */
#define LL_GROUPBY_HASH_MIN 32

typedef struct {
    uint32_t sym;
    Value *items;
    size_t len, cap;
} GroupBucket;

static Value groupBy2_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value f = args[0], list = args[1];
    LL_REQUIRE_LIST(list, "groupBy");
    int64_t n = ll_bshared_list_count(list);
    const Value *elems = ll_bshared_list_elems(list);
    GroupBucket *buckets = NULL;
    size_t nbuckets = 0, bcap = 0;

    int use_hash = n > LL_GROUPBY_HASH_MIN;
    size_t hcap = 0;
    uint32_t *hsym = NULL;  /* hcap entries, parallel to hused/hbucket */
    size_t *hbucket = NULL;
    uint8_t *hused = NULL;
    if (use_hash) {
        hcap = 16;
        while (hcap < (size_t)n * 2) hcap <<= 1;
        hsym = malloc(hcap * sizeof(uint32_t));
        hbucket = malloc(hcap * sizeof(size_t));
        hused = calloc(hcap, 1);
    }

    for (int64_t i = 0; i < n; i++) {
        Value key = ll_apply(f, &elems[i], 1);
        key = ll_force_for_print(key);
        if (ll_tag_of(key) != LL_TAG_PTR || ll_value_is_path(key) || ll_value_is_float(key)) {
            fprintf(stderr, "ll: groupBy: key function must return a string\n");
            exit(1);
        }
        int64_t klen;
        const char *kbytes = ll_ptr_bytes(key, &klen);
        uint32_t sym = ll_intern(kbytes, (size_t)klen);
        GroupBucket *b = NULL;
        size_t hslot = 0;
        if (use_hash) {
            size_t mask = hcap - 1;
            hslot = ((size_t)sym * 2654435761u) & mask;
            while (hused[hslot]) {
                if (hsym[hslot] == sym) {
                    b = &buckets[hbucket[hslot]];
                    break;
                }
                hslot = (hslot + 1) & mask;
            }
        } else {
            for (size_t j = 0; j < nbuckets; j++) {
                if (buckets[j].sym == sym) {
                    b = &buckets[j];
                    break;
                }
            }
        }
        if (!b) {
            if (nbuckets == bcap) {
                bcap = bcap ? bcap * 2 : 8;
                buckets = realloc(buckets, bcap * sizeof(GroupBucket));
            }
            size_t new_idx = nbuckets;
            b = &buckets[nbuckets++];
            b->sym = sym;
            b->items = NULL;
            b->len = 0;
            b->cap = 0;
            if (use_hash) {
                hused[hslot] = 1;
                hsym[hslot] = sym;
                hbucket[hslot] = new_idx;
            }
        }
        if (b->len == b->cap) {
            b->cap = b->cap ? b->cap * 2 : 8;
            b->items = realloc(b->items, b->cap * sizeof(Value));
        }
        b->items[b->len++] = elems[i];
    }
    if (use_hash) {
        free(hsym);
        free(hbucket);
        free(hused);
    }
    uint32_t *syms = nbuckets > 0 ? malloc(nbuckets * sizeof(uint32_t)) : NULL;
    Value *vals = nbuckets > 0 ? malloc(nbuckets * sizeof(Value)) : NULL;
    for (size_t j = 0; j < nbuckets; j++) {
        syms[j] = buckets[j].sym;
        vals[j] = ll_bshared_make_list(buckets[j].items, (int64_t)buckets[j].len);
        free(buckets[j].items);
    }
    free(buckets);
    Value result = ll_bshared_make_attrs(syms, vals, (int64_t)nbuckets);
    free(syms);
    free(vals);
    return result;
}
static Value build_groupBy(void) { return ll_bshared_make_curried_native((void *)groupBy2_impl, 2); }

/* `builtins.parseDrvName s` -- splits a derivation-name-style string
 * into `{ name; version; }`, ported from real Nix's own
 * `DrvName::DrvName`: the split point is the FIRST '-' immediately
 * followed by a non-letter (not just any '-'), so "xf86-video-i810-1.7.4"
 * splits at the dash before "1.7.4" (the two earlier dashes are
 * followed by letters, so skipped), and a doubled "--" splits at the
 * FIRST of the pair (since '-' itself isn't a letter). No matching dash
 * at all means the whole string is the name, version "". */
static Value prim_parseDrvName(const Value *args, int64_t argc) {
    (void)argc;
    LL_REQUIRE_STRING(args[0], "parseDrvName");
    int64_t len;
    const char *s = ll_ptr_bytes(args[0], &len);
    int64_t split = -1;
    for (int64_t i = 0; i < len - 1; i++) {
        if (s[i] == '-' && !isalpha((unsigned char)s[i + 1])) {
            split = i;
            break;
        }
    }
    Value name_val, version_val;
    if (split < 0) {
        name_val = ll_make_string_value(s, (size_t)len);
        version_val = ll_make_string_value("", 0);
    } else {
        name_val = ll_make_string_value(s, (size_t)split);
        version_val = ll_make_string_value(s + split + 1, (size_t)(len - split - 1));
    }
    uint32_t syms[2] = { ll_intern("name", 4), ll_intern("version", 7) };
    Value vals[2] = { name_val, version_val };
    /* order doesn't matter -- ll_bshared_make_attrs sorts by sym id */
    return ll_bshared_make_attrs(syms, vals, 2);
}

/* `builtins.add`/`sub`/`mul`/`div` -- plain two-argument arithmetic,
 * exposed as builtins (distinct from the `+`/`-`/`*`/`/` *operators*,
 * which compile to IR ops directly). Shares the same int/float
 * promotion rule the operators use (ll_add_values/ll_sub_values/
 * ll_mul_values/ll_div_values, value_util.c) rather than being
 * INT-only. */
static Value add2_impl(const Value *args, int64_t argc) {
    (void)argc;
    return ll_add_values(args[0], args[1]);
}
static Value sub2_impl(const Value *args, int64_t argc) {
    (void)argc;
    if (ll_tag_of(args[0]) == LL_TAG_INT && ll_tag_of(args[1]) == LL_TAG_INT) {
        return ll_make_int(ll_int_value(args[0]) - ll_int_value(args[1]));
    }
    return ll_sub_values(args[0], args[1]);
}
static Value mul2_impl(const Value *args, int64_t argc) {
    (void)argc;
    if (ll_tag_of(args[0]) == LL_TAG_INT && ll_tag_of(args[1]) == LL_TAG_INT) {
        /* Unlike add/sub (operands already bounded to +-2^60, so plain
         * int64 `+`/`-` can never themselves overflow), two such operands'
         * *product* genuinely can overflow int64 -- true signed-overflow
         * UB in C, not just a value ll_make_int could catch after the
         * fact. __builtin_mul_overflow computes the product without that
         * UB and reports it; ll_make_int still re-checks the narrower
         * +-2^60 window for a product that fits in 64 bits but not that
         * window. Do not replace this with a plain `a * b`. */
        int64_t a = ll_int_value(args[0]), b = ll_int_value(args[1]), r;
        if (__builtin_mul_overflow(a, b, &r)) {
            fprintf(stderr, "ll: integer overflow in multiplying %lld * %lld\n", (long long)a, (long long)b);
            exit(1);
        }
        return ll_make_int(r);
    }
    return ll_mul_values(args[0], args[1]);
}
static Value div2_impl(const Value *args, int64_t argc) {
    (void)argc;
    if (ll_tag_of(args[0]) == LL_TAG_INT && ll_tag_of(args[1]) == LL_TAG_INT) {
        int64_t rhs = ll_int_value(args[1]);
        if (rhs == 0) {
            fprintf(stderr, "ll: division by zero, aborting\n");
            exit(1);
        }
        return ll_make_int(ll_int_value(args[0]) / rhs);
    }
    return ll_div_values(args[0], args[1]);
}
static Value build_add(void) { return ll_bshared_make_curried_native((void *)add2_impl, 2); }
static Value build_sub(void) { return ll_bshared_make_curried_native((void *)sub2_impl, 2); }
static Value build_mul(void) { return ll_bshared_make_curried_native((void *)mul2_impl, 2); }
static Value build_div(void) { return ll_bshared_make_curried_native((void *)div2_impl, 2); }

const LlBuiltinMiscDef LL_M23_BUILTINS[] = {
    { "seq", build_seq },
    { "deepSeq", build_deepSeq },
    { "intersectAttrs", build_intersectAttrs },
    { "groupBy", build_groupBy },
    { "add", build_add },
    { "sub", build_sub },
    { "mul", build_mul },
    { "div", build_div },
    { "hashFile", build_hashFile },
};

const size_t LL_M23_BUILTINS_COUNT = sizeof(LL_M23_BUILTINS) / sizeof(LL_M23_BUILTINS[0]);

Value ll_prim_pathExists(const Value *args, int64_t argc) { return prim_pathExists(args, argc); }
Value ll_prim_readFile(const Value *args, int64_t argc) { return prim_readFile(args, argc); }
Value ll_prim_readFileType(const Value *args, int64_t argc) { return prim_readFileType(args, argc); }
Value ll_prim_readDir(const Value *args, int64_t argc) { return prim_readDir(args, argc); }
Value ll_prim_toPath(const Value *args, int64_t argc) { return prim_toPath(args, argc); }
Value ll_prim_getEnv(const Value *args, int64_t argc) { return prim_getEnv(args, argc); }
Value ll_prim_parseDrvName(const Value *args, int64_t argc) { return prim_parseDrvName(args, argc); }
