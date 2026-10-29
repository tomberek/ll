/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#define _DEFAULT_SOURCE /* lstat()/strdup()/readlink() -- same feature-test macro value_util.c uses */

#include "nar.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "derivation.h" /* ll_compute_fixed_output_path, shared with builtins.path's formula */
#include "hash_shim.h"
#include "store_path.h"

typedef struct {
    char *buf;
    size_t len, cap;
} NarBuf;

static void nb_reserve(NarBuf *b, size_t extra) {
    if (b->len + extra <= b->cap) return;
    b->cap = b->cap ? b->cap * 2 : 4096;
    while (b->len + extra > b->cap) b->cap *= 2;
    b->buf = realloc(b->buf, b->cap);
}

static void nb_bytes(NarBuf *b, const void *p, size_t n) {
    nb_reserve(b, n);
    memcpy(b->buf + b->len, p, n);
    b->len += n;
}

/* Little-endian 8-byte length, matching real Nix's own
 * `operator<<(Sink&, uint64_t)` (~/nix/src/libutil/include/nix/util/
 * serialise.hh) exactly. */
static void nb_u64(NarBuf *b, uint64_t n) {
    unsigned char buf[8];
    for (int i = 0; i < 8; i++) buf[i] = (unsigned char)(n >> (8 * i));
    nb_bytes(b, buf, 8);
}

/* NAR string field: 8-byte length, raw bytes, zero-padded to the next
 * 8-byte boundary (~/nix/src/libutil/serialise.cc's `writeString`). */
static void nb_str(NarBuf *b, const char *s, size_t n) {
    nb_u64(b, (uint64_t)n);
    nb_bytes(b, s, n);
    size_t pad = (8 - (n % 8)) % 8;
    if (pad) {
        static const char zero[8] = { 0 };
        nb_bytes(b, zero, pad);
    }
}

static void nb_cstr(NarBuf *b, const char *s) {
    nb_str(b, s, strlen(s));
}

#define LL_NAR_MAX_DEPTH 256

static int name_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Real Nix's `FileType` names (~/nix/src/libutil/file-system.hh's
 * `InputAccessor::Type` -> `dumpPath`'s own filter calls), used both for
 * `nar_dump`'s own filter calls below and matching exactly what a Nix-
 * level `filter = path: type: ...` function receives. */
static const char *nar_file_type_name(mode_t mode) {
    if (S_ISREG(mode)) return "regular";
    if (S_ISDIR(mode)) return "directory";
    if (S_ISLNK(mode)) return "symlink";
    return "unknown";
}

static void nar_dump(NarBuf *b, const char *path, int depth, LlNarFilter filter, void *filter_ctx) {
    if (depth >= LL_NAR_MAX_DEPTH) {
        fprintf(stderr, "ll: path '%s' exceeds maximum NAR directory depth of %d\n", path, LL_NAR_MAX_DEPTH);
        exit(1);
    }

    struct stat st;
    if (lstat(path, &st) != 0) {
        fprintf(stderr, "ll: cannot stat '%s' while copying it to the store\n", path);
        exit(1);
    }

    nb_cstr(b, "(");

    if (S_ISREG(st.st_mode)) {
        nb_cstr(b, "type");
        nb_cstr(b, "regular");
        if (st.st_mode & S_IXUSR) {
            nb_cstr(b, "executable");
            nb_cstr(b, "");
        }
        nb_cstr(b, "contents");
        FILE *f = fopen(path, "rb");
        if (!f) {
            fprintf(stderr, "ll: cannot open '%s' while copying it to the store\n", path);
            exit(1);
        }
        uint64_t size = (uint64_t)st.st_size;
        nb_u64(b, size);
        char chunk[65536];
        uint64_t remaining = size;
        while (remaining > 0) {
            size_t want = remaining < sizeof(chunk) ? (size_t)remaining : sizeof(chunk);
            size_t got = fread(chunk, 1, want, f);
            if (got == 0) break;
            nb_bytes(b, chunk, got);
            remaining -= got;
        }
        fclose(f);
        size_t pad = (8 - (size % 8)) % 8;
        if (pad) {
            static const char zero[8] = { 0 };
            nb_bytes(b, zero, pad);
        }
    } else if (S_ISDIR(st.st_mode)) {
        nb_cstr(b, "type");
        nb_cstr(b, "directory");

        DIR *d = opendir(path);
        if (!d) {
            fprintf(stderr, "ll: cannot read directory '%s' while copying it to the store\n", path);
            exit(1);
        }
        char **names = NULL;
        size_t nnames = 0, cap = 0;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            if (nnames == cap) {
                cap = cap ? cap * 2 : 16;
                names = realloc(names, cap * sizeof(char *));
            }
            names[nnames++] = strdup(de->d_name);
        }
        closedir(d);
        qsort(names, nnames, sizeof(char *), name_cmp);

        for (size_t i = 0; i < nnames; i++) {
            char child[4096];
            snprintf(child, sizeof(child), "%s/%s", path, names[i]);
            /* `filter` (NULL from `ll_copy_path_to_store`'s own call
             * below, in which case every "entry"/"node" pair is emitted
             * unconditionally) decides whether this child (and, if it's
             * a directory, its ENTIRE subtree -- simply never recursed
             * into, same as real Nix's own `PathFilter`) appears in the
             * NAR at all. */
            if (filter) {
                struct stat cst;
                if (lstat(child, &cst) != 0) {
                    fprintf(stderr, "ll: cannot stat '%s' while copying it to the store\n", child);
                    exit(1);
                }
                if (!filter(filter_ctx, child, nar_file_type_name(cst.st_mode))) {
                    free(names[i]);
                    continue;
                }
            }
            nb_cstr(b, "entry");
            nb_cstr(b, "(");
            nb_cstr(b, "name");
            nb_cstr(b, names[i]);
            nb_cstr(b, "node");
            nar_dump(b, child, depth + 1, filter, filter_ctx);
            nb_cstr(b, ")");
            free(names[i]);
        }
        free(names);
    } else if (S_ISLNK(st.st_mode)) {
        nb_cstr(b, "type");
        nb_cstr(b, "symlink");
        nb_cstr(b, "target");
        char target[4096];
        ssize_t n = readlink(path, target, sizeof(target) - 1);
        if (n < 0) {
            fprintf(stderr, "ll: cannot read symlink '%s' while copying it to the store\n", path);
            exit(1);
        }
        target[n] = '\0';
        nb_cstr(b, target);
    } else {
        fprintf(stderr, "ll: file '%s' has an unsupported type for copying to the store\n", path);
        exit(1);
    }

    nb_cstr(b, ")");
}

typedef struct {
    char *path;
    char *result;
} CopyCacheEntry;
static CopyCacheEntry *g_copy_cache = NULL;
static size_t g_copy_cache_count = 0, g_copy_cache_cap = 0;

/* Open-addressing dedup index parallel to g_copy_cache, giving O(1)
 * amortized lookup instead of a linear strcmp scan over every distinct
 * path copied so far this eval. Local copy of derivation.c's
 * dcc_hash_str (not shared via a header -- pure internal
 * bucket-placement hash, never persisted, so each copy is free to
 * diverge). */
static uint64_t copy_cache_hash_str(const char *s, size_t len) {
    uint64_t h = 0;
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t word;
        memcpy(&word, s + i, 8);
        h ^= word;
        h *= 0x517cc1b727220a95ULL;
    }
    for (; i < len; i++) {
        h ^= (uint64_t)(unsigned char)s[i];
        h *= 0x517cc1b727220a95ULL;
    }
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebULL;
    h ^= h >> 31;
    return h;
}

#define COPY_CACHE_EMPTY_SLOT SIZE_MAX
static size_t *g_copy_cache_index = NULL;
static size_t g_copy_cache_index_cap = 0;

static void copy_cache_index_rehash(size_t new_cap) {
    size_t *new_table = malloc(new_cap * sizeof(size_t));
    for (size_t i = 0; i < new_cap; i++) new_table[i] = COPY_CACHE_EMPTY_SLOT;
    for (size_t i = 0; i < g_copy_cache_count; i++) {
        size_t slot = copy_cache_hash_str(g_copy_cache[i].path, strlen(g_copy_cache[i].path)) & (new_cap - 1);
        while (new_table[slot] != COPY_CACHE_EMPTY_SLOT) slot = (slot + 1) & (new_cap - 1);
        new_table[slot] = i;
    }
    free(g_copy_cache_index);
    g_copy_cache_index = new_table;
    g_copy_cache_index_cap = new_cap;
}

static const char *copy_cache_find(const char *path) {
    if (g_copy_cache_index_cap == 0) return NULL;
    size_t len = strlen(path);
    size_t slot = copy_cache_hash_str(path, len) & (g_copy_cache_index_cap - 1);
    for (;;) {
        size_t idx = g_copy_cache_index[slot];
        if (idx == COPY_CACHE_EMPTY_SLOT) return NULL;
        if (strcmp(g_copy_cache[idx].path, path) == 0) return g_copy_cache[idx].result;
        slot = (slot + 1) & (g_copy_cache_index_cap - 1);
    }
}

static void copy_cache_insert(const char *path, const char *result) {
    if (g_copy_cache_index_cap == 0) copy_cache_index_rehash(16);
    else if ((g_copy_cache_count + 1) * 4 > g_copy_cache_index_cap * 3) copy_cache_index_rehash(g_copy_cache_index_cap * 2);
    if (g_copy_cache_count == g_copy_cache_cap) {
        g_copy_cache_cap = g_copy_cache_cap ? g_copy_cache_cap * 2 : 16;
        g_copy_cache = realloc(g_copy_cache, g_copy_cache_cap * sizeof(*g_copy_cache));
    }
    g_copy_cache[g_copy_cache_count].path = strdup(path);
    g_copy_cache[g_copy_cache_count].result = strdup(result);
    size_t slot = copy_cache_hash_str(path, strlen(path)) & (g_copy_cache_index_cap - 1);
    while (g_copy_cache_index[slot] != COPY_CACHE_EMPTY_SLOT) slot = (slot + 1) & (g_copy_cache_index_cap - 1);
    g_copy_cache_index[slot] = g_copy_cache_count;
    g_copy_cache_count++;
}

const char *ll_copy_path_to_store_cache_lookup(const char *path) {
    return copy_cache_find(path);
}

void ll_copy_path_to_store(const char *path, char *out, size_t out_cap) {
    const char *cached = copy_cache_find(path);
    if (cached) {
        snprintf(out, out_cap, "%s", cached);
        return;
    }

    NarBuf b = { 0 };
    nb_cstr(&b, "nix-archive-1");
    nar_dump(&b, path, 0, NULL, NULL);

    uint8_t nar_hash[32];
    if (ll_hash_sha256(b.buf, b.len, nar_hash) != 0) {
        fprintf(stderr, "ll: failed to load libcrypto -- enter the flake devShell\n");
        exit(1);
    }
    free(b.buf);

    /* Real Nix's `baseNameOf` -- the last '/'-separated component,
     * same as every other `name` this project derives from a path. A
     * trailing slash isn't expected here: every path reaching this
     * function was already canonicalized by the parser/OP_PATH_CONCAT
     * before being coerced. */
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;

    static const char hexdigits[] = "0123456789abcdef";
    char hashstr[7 + 64 + 1];
    memcpy(hashstr, "sha256:", 7);
    for (int i = 0; i < 32; i++) {
        hashstr[7 + 2 * i] = hexdigits[nar_hash[i] >> 4];
        hashstr[7 + 2 * i + 1] = hexdigits[nar_hash[i] & 0xf];
    }
    hashstr[7 + 64] = '\0';

    size_t preimage_cap = strlen("source") + strlen(hashstr) + strlen(LL_STORE_DIR) + strlen(name) + 8;
    char *preimage = malloc(preimage_cap);
    size_t n = ll_store_path_preimage("source", hashstr, name, preimage, preimage_cap);
    if (n == 0) {
        fprintf(stderr, "ll: preimage too long copying '%s' to the store\n", path);
        exit(1);
    }
    uint8_t outer_digest[32];
    if (ll_hash_sha256(preimage, n, outer_digest) != 0) {
        fprintf(stderr, "ll: failed to load libcrypto -- enter the flake devShell\n");
        exit(1);
    }
    free(preimage);

    if (ll_make_store_path_from_digest(outer_digest, name, out, out_cap) == 0) {
        fprintf(stderr, "ll: computed store path too long copying '%s' to the store\n", path);
        exit(1);
    }

    copy_cache_insert(path, out);
}

void ll_copy_path_to_store_ext(const char *path, const char *name, int recursive, LlNarFilter filter,
                                   void *filter_ctx, const uint8_t expected_sha256[32], char *out, size_t out_cap) {
    const char *slash = strrchr(path, '/');
    const char *effective_name = name ? name : (slash ? slash + 1 : path);

    uint8_t content_digest[32];
    if (recursive) {
        NarBuf b = { 0 };
        nb_cstr(&b, "nix-archive-1");
        nar_dump(&b, path, 0, filter, filter_ctx);
        if (ll_hash_sha256(b.buf, b.len, content_digest) != 0) {
            fprintf(stderr, "ll: failed to load libcrypto -- enter the flake devShell\n");
            exit(1);
        }
        free(b.buf);
    } else {
        /* Flat method -- real Nix requires `path` be a regular file
         * (`builtins.path { recursive = false; ... }` on a directory is
         * a hard error there); no filter applies (there's nothing to
         * recurse into). */
        struct stat st;
        if (lstat(path, &st) != 0) {
            fprintf(stderr, "ll: path '%s' does not exist\n", path);
            exit(1);
        }
        if (!S_ISREG(st.st_mode)) {
            fprintf(stderr, "ll: path '%s' is not a regular file (required for a non-recursive hash)\n", path);
            exit(1);
        }
        FILE *f = fopen(path, "rb");
        if (!f) {
            fprintf(stderr, "ll: cannot open '%s'\n", path);
            exit(1);
        }
        char *buf = malloc((size_t)st.st_size ? (size_t)st.st_size : 1);
        size_t got = fread(buf, 1, (size_t)st.st_size, f);
        fclose(f);
        if (ll_hash_sha256(buf, got, content_digest) != 0) {
            fprintf(stderr, "ll: failed to load libcrypto -- enter the flake devShell\n");
            exit(1);
        }
        free(buf);
    }

    if (expected_sha256 && memcmp(expected_sha256, content_digest, 32) != 0) {
        fprintf(stderr, "ll: hash mismatch copying '%s' to the store (sha256 does not match the pinned value)\n",
                path);
        exit(1);
    }

    ll_compute_fixed_output_path(recursive ? "recursive" : "flat", content_digest, 32, "sha256", 6,
                                     effective_name, out, out_cap);
}

int ll_hash_path_nar_sha256(const char *path, LlNarFilter filter, void *filter_ctx, uint8_t out[32]) {
    NarBuf b = { 0 };
    nb_cstr(&b, "nix-archive-1");
    nar_dump(&b, path, 0, filter, filter_ctx);
    int rc = ll_hash_sha256(b.buf, b.len, out);
    free(b.buf);
    return rc;
}
