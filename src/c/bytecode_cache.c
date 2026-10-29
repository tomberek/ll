/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445's src/c/bytecode_cache.c -- see ~/.claude/plans/soft-wishing-engelbart.md.
 *
 * KNOWN INTERACTION with `ll`'s own lazy-thunk-body-compile feature
 * (default ON here, unlike asnix which keeps it opt-in): a freshly-
 * compiled program can contain not-yet-expanded OP_LAZY_THUNK_BODY
 * stub thunks (bodies deferred until first force) at the exact moment
 * eval_file.c calls ll_bytecode_cache_store right after compiling --
 * bytecode.c's own program_op_count explicitly refuses to serialize
 * ANY program containing one (a LazyThunkDescriptor's `body`/`scope`
 * are compile-time-only Ast/ScopeFrame pointers, meaningless cross-
 * process), and that refusal propagates all the way up through
 * write_program's OP_MK_THUNK recursion, failing the WHOLE top-level
 * program's cache store even if only one nested thunk is still a stub.
 * Not a correctness bug (every store attempt that DOES go through, or
 * any run with the cache enabled at all, still produces byte-identical
 * output either way); just a real hit-rate cost. Set LL_EAGER_THUNK=1
 * for a meaningful hit rate when LL_BYTECODE_CACHE=1 is also set. */
#define _DEFAULT_SOURCE
#include "bytecode_cache.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "ast.h"
#include "bytecode.h"
#include "hash_shim.h"

int ll_bytecode_cache_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("LL_BYTECODE_CACHE");
        cached = (v && strcmp(v, "1") == 0) ? 1 : 0;
    }
    return cached;
}

/* Memoized: the env vars this reads (LL_BYTECODE_CACHE_DIR/HOME) are
 * fixed for the process lifetime. The returned pointer is a
 * process-lifetime singleton, never freed by the caller. */
static char *cache_dir(void) {
    static char *cached = NULL;
    if (cached) return cached;
    const char *override = getenv("LL_BYTECODE_CACHE_DIR");
    if (override) {
        cached = strdup(override);
        return cached;
    }
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    size_t n = strlen(home) + strlen("/.cache/ll/bytecode") + 1;
    char *p = malloc(n);
    snprintf(p, n, "%s/.cache/ll/bytecode", home);
    cached = p;
    return cached;
}

static int errno_is_exist(void) {
    return errno == EEXIST;
}

static int mkdir_p(const char *path) {
    char *copy = strdup(path);
    size_t len = strlen(copy);
    for (size_t i = 1; i < len; i++) {
        if (copy[i] == '/') {
            copy[i] = '\0';
            mkdir(copy, 0755); /* ignore EEXIST and any other failure here --
                                 * the final mkdir below is the one whose
                                 * result actually matters */
            copy[i] = '/';
        }
    }
    int rc = mkdir(copy, 0755);
    free(copy);
    return (rc == 0 || errno_is_exist()) ? 0 : -1;
}

/* The on-disk FILENAME key is sha256(resolved_path) alone, hex-encoded
 * -- NOT (path, content). Two different paths never collide; freshness
 * against whatever content currently lives at this path is validated
 * separately, by the entry's own header (CacheHeader below), not by
 * the filename. */
static void path_hash_hex(const char *resolved_path, char out_hex[64]) {
    unsigned char digest[LL_HASH_SHA256_LEN];
    int64_t rc = ll_hash_sha256(resolved_path, strlen(resolved_path), digest);
    if (rc != 0) {
        memset(out_hex, '0', 64);
        return;
    }
    ll_hash_to_hex(digest, LL_HASH_SHA256_LEN, out_hex);
}

static char *cache_file_path(const char *resolved_path) {
    char hex[64];
    path_hash_hex(resolved_path, hex);
    char *dir = cache_dir(); /* memoized singleton -- do not free */
    size_t n = strlen(dir) + 1 + 2 + 1 + 64 + 4 + 1;
    char *p = malloc(n);
    snprintf(p, n, "%s/%.2s/%.64s.abc", dir, hex, hex);
    return p;
}

/* Cache-entry header -- mtime+size (the cheap freshness check) and a
 * content hash (the fallback check), stored ahead of the entry's
 * bytecode.c-serialized body. Fixed-width, hand-rolled (not bytecode.c's
 * own WBuf/varint machinery -- this is the cache LAYER wrapping that
 * format, not part of the format itself). `CACHE_FORMAT_MAGIC` is a
 * distinct `ll`-own magic (not asnix's "ANXCACH1") so an `ll`-written
 * entry and an asnix-written entry sharing a $HOME can never be
 * cross-read as if they were the other's. */
#define CACHE_FORMAT_MAGIC "LLBCACH1"
#define CACHE_FORMAT_VERSION ((uint8_t)1)

typedef struct {
    int64_t mtime_sec, mtime_nsec, size;
    unsigned char content_hash[LL_HASH_SHA256_LEN];
} CacheHeader;

static int write_header(FILE *f, const CacheHeader *h) {
    uint8_t pad[7] = { 0 };
    if (fwrite(CACHE_FORMAT_MAGIC, 1, 8, f) != 8) return -1;
    uint8_t ver = CACHE_FORMAT_VERSION;
    if (fwrite(&ver, 1, 1, f) != 1) return -1;
    if (fwrite(pad, 1, 7, f) != 7) return -1;
    if (fwrite(&h->mtime_sec, sizeof h->mtime_sec, 1, f) != 1) return -1;
    if (fwrite(&h->mtime_nsec, sizeof h->mtime_nsec, 1, f) != 1) return -1;
    if (fwrite(&h->size, sizeof h->size, 1, f) != 1) return -1;
    if (fwrite(h->content_hash, 1, LL_HASH_SHA256_LEN, f) != LL_HASH_SHA256_LEN) return -1;
    return 0;
}

/* Exactly 8(magic)+1(ver)+7(pad)+8(mtime_sec)+8(mtime_nsec)+8(size)+
 * 32(content_hash) = 72 bytes, every multi-byte field on an 8-byte
 * boundary -- fixed and self-contained, independent of this struct's
 * own in-memory padding on any given platform. */
#define CACHE_HEADER_LEN 72

/* Parses the header directly out of an mmap'd buffer instead of
 * `fread`ing it field-by-field through a `FILE*` -- paired with
 * `mmap_whole_file` below, this avoids a cache HIT ever copying the
 * entry's bytes into a heap buffer at all. `memcpy` (not a direct
 * pointer-cast read) for the multi-byte fields, since `buf` is only
 * guaranteed byte-aligned, not aligned to each field's own type. */
static int parse_header_from_buf(const uint8_t *buf, size_t len, CacheHeader *h) {
    if (len < CACHE_HEADER_LEN) return -1;
    if (memcmp(buf, CACHE_FORMAT_MAGIC, 8) != 0) return -1;
    if (buf[8] != CACHE_FORMAT_VERSION) return -1;
    memcpy(&h->mtime_sec, buf + 16, sizeof h->mtime_sec);
    memcpy(&h->mtime_nsec, buf + 24, sizeof h->mtime_nsec);
    memcpy(&h->size, buf + 32, sizeof h->size);
    memcpy(h->content_hash, buf + 40, LL_HASH_SHA256_LEN);
    return 0;
}

/* Maps the WHOLE cache-entry file read-only, letting the caller read
 * the header AND hand the body straight to `ll_bytecode_deserialize`
 * with no intervening copy. Returns NULL on any failure (missing file,
 * empty file, mmap failure); the fd is always closed before returning. */
static const uint8_t *mmap_whole_file(const char *path, size_t *out_len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return NULL;
    }
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return NULL;
    *out_len = (size_t)st.st_size;
    return (const uint8_t *)p;
}

/* LL_BYTECODE_CACHE_STATS=1 (on top of LL_BYTECODE_CACHE=1) prints a
 * one-line hit/miss/store summary to stderr at process exit -- the
 * cache is otherwise silent by design. */
static int64_t g_stat_hits = 0, g_stat_fast_hits = 0, g_stat_verify_hits = 0, g_stat_misses = 0, g_stat_stores = 0;
static int g_stats_atexit_registered = 0;

static int stats_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("LL_BYTECODE_CACHE_STATS");
        cached = (v && strcmp(v, "1") == 0) ? 1 : 0;
    }
    return cached;
}

static void print_stats_atexit(void) {
    int64_t total = g_stat_hits + g_stat_misses;
    double rate = total > 0 ? (100.0 * (double)g_stat_hits / (double)total) : 0.0;
    fprintf(stderr,
            "ll: bytecode cache: %lld hits (%lld fast-path, %lld content-verified), %lld misses, %lld stores "
            "(%.1f%% hit rate)\n",
            (long long)g_stat_hits, (long long)g_stat_fast_hits, (long long)g_stat_verify_hits,
            (long long)g_stat_misses, (long long)g_stat_stores, rate);
}

static void ensure_stats_atexit(void) {
    if (!stats_enabled() || g_stats_atexit_registered) return;
    g_stats_atexit_registered = 1;
    atexit(print_stats_atexit);
}

/* Writes a complete entry (header + body) atomically via write-temp-
 * then-rename -- shared by ll_bytecode_cache_store itself and by
 * ll_bytecode_cache_lookup_verify's own "self-heal" (re-stamp an
 * entry with fresh mtime/size once its content is confirmed unchanged,
 * so the FAST path starts working again next time). */
static void write_entry_atomic(const char *path, const CacheHeader *h, const uint8_t *body, size_t body_len) {
    char *dir_copy = strdup(path);
    char *slash = strrchr(dir_copy, '/');
    if (slash) {
        *slash = '\0';
        mkdir_p(dir_copy);
    }
    free(dir_copy);
    size_t tmp_len = strlen(path) + 32;
    char *tmp = malloc(tmp_len);
    snprintf(tmp, tmp_len, "%s.tmp%d", path, (int)getpid());
    FILE *f = fopen(tmp, "wb");
    if (f) {
        int ok = write_header(f, h) == 0;
        size_t wrote = ok ? fwrite(body, 1, body_len, f) : 0;
        fclose(f);
        if (ok && wrote == body_len) {
            rename(tmp, path);
            g_stat_stores++;
        } else {
            unlink(tmp);
        }
    }
    free(tmp);
}

/* Step 1 of the fast/verify lookup split: no source-file I/O at all,
 * just one open+read of the cache entry itself (or, on a plain miss,
 * not even that beyond the failed open). */
IrProgram *ll_bytecode_cache_lookup_fast(const char *resolved_path, int64_t mtime_sec, int64_t mtime_nsec,
                                             int64_t size, int *out_entry_exists) {
    *out_entry_exists = 0;
    if (!ll_bytecode_cache_enabled()) return NULL;
    ensure_stats_atexit();
    char *path = cache_file_path(resolved_path);
    size_t total_len;
    const uint8_t *map = mmap_whole_file(path, &total_len);
    free(path);
    if (!map) {
        g_stat_misses++;
        return NULL;
    }
    CacheHeader hdr;
    if (parse_header_from_buf(map, total_len, &hdr) != 0) {
        munmap((void *)map, total_len);
        g_stat_misses++;
        return NULL;
    }
    *out_entry_exists = 1;
    if (hdr.mtime_sec != mtime_sec || hdr.mtime_nsec != mtime_nsec || hdr.size != size) {
        /* Stat drifted -- ambiguous, not necessarily stale (could be a
         * touch(1) with unchanged content). Caller should try the
         * content-hash fallback (ll_bytecode_cache_lookup_verify)
         * rather than treating this as an outright miss yet. */
        munmap((void *)map, total_len);
        return NULL;
    }
    const uint8_t *body = map + CACHE_HEADER_LEN;
    size_t body_len = total_len - CACHE_HEADER_LEN;
    IrProgram *prog = ll_bytecode_deserialize(body, body_len);
    munmap((void *)map, total_len);
    if (prog) {
        g_stat_hits++;
        g_stat_fast_hits++;
    } else {
        g_stat_misses++;
    }
    return prog; /* NULL on any anomaly -- ll_bytecode_deserialize's own contract */
}

/* Step 2 -- only called by eval_file.c when step 1 above found an
 * entry whose mtime/size didn't match. Needs the actual source bytes
 * (the caller reads these anyway on the way to an ordinary cache-miss
 * recompile) to compute a real content hash and compare it against the
 * one recorded in the SAME entry step 1 already found. */
IrProgram *ll_bytecode_cache_lookup_verify(const char *resolved_path, const char *source, size_t source_len,
                                               int64_t mtime_sec, int64_t mtime_nsec, int64_t size) {
    if (!ll_bytecode_cache_enabled()) return NULL;
    ensure_stats_atexit();
    char *path = cache_file_path(resolved_path);
    size_t total_len;
    const uint8_t *map = mmap_whole_file(path, &total_len);
    if (!map) {
        free(path);
        g_stat_misses++;
        return NULL;
    }
    CacheHeader hdr;
    if (parse_header_from_buf(map, total_len, &hdr) != 0) {
        munmap((void *)map, total_len);
        free(path);
        g_stat_misses++;
        return NULL;
    }
    unsigned char digest[LL_HASH_SHA256_LEN];
    if (ll_hash_sha256(source, source_len, digest) != 0 ||
        memcmp(digest, hdr.content_hash, LL_HASH_SHA256_LEN) != 0) {
        /* Either couldn't hash, or content genuinely differs -- a real
         * miss; the caller will recompile and ll_bytecode_cache_store
         * will overwrite this stale entry. */
        munmap((void *)map, total_len);
        free(path);
        g_stat_misses++;
        return NULL;
    }
    const uint8_t *body = map + CACHE_HEADER_LEN;
    size_t body_len = total_len - CACHE_HEADER_LEN;
    IrProgram *prog = ll_bytecode_deserialize(body, body_len);
    if (prog) {
        g_stat_hits++;
        g_stat_verify_hits++;
        /* Self-heal: content is confirmed unchanged, so re-stamp this
         * entry with the FRESH mtime/size the caller just stat'd --
         * restores the fast path for this exact path on the next
         * lookup. `body` is still a valid pointer into the mapping at
         * this point (munmap happens below, after this call) --
         * write_entry_atomic only ever reads from it, never retains it
         * past its own return. */
        CacheHeader fresh = hdr;
        fresh.mtime_sec = mtime_sec;
        fresh.mtime_nsec = mtime_nsec;
        fresh.size = size;
        write_entry_atomic(path, &fresh, body, body_len);
    } else {
        g_stat_misses++;
    }
    munmap((void *)map, total_len);
    free(path);
    return prog;
}

void ll_bytecode_cache_store(const char *resolved_path, const char *source, size_t source_len, int64_t mtime_sec,
                                 int64_t mtime_nsec, int64_t size, IrProgram *prog) {
    if (!ll_bytecode_cache_enabled()) return;
    ensure_stats_atexit();
    uint8_t *buf;
    size_t len;
    if (ll_bytecode_serialize(prog, &buf, &len) != 0) return; /* refused -- some
                                                                    * construct this
                                                                    * format doesn't
                                                                    * support; just
                                                                    * skip caching it */
    CacheHeader h;
    h.mtime_sec = mtime_sec;
    h.mtime_nsec = mtime_nsec;
    h.size = size;
    if (ll_hash_sha256(source, source_len, h.content_hash) != 0) {
        free(buf);
        return; /* couldn't hash -- skip caching this one rather than
                  * store an entry with an undefined content hash */
    }
    char *path = cache_file_path(resolved_path);
    write_entry_atomic(path, &h, buf, len);
    free(path);
    free(buf);
}

/* Deliberately not ported: asnix's io_uring-based prefetch-worker pool
 * is a separate feature layered on top of this cache, not required for
 * the cache's own hit/miss behavior. Honest no-ops -- every
 * eval_file.c call site already treats these as "maybe does nothing." */
void ll_bytecode_prefetch_imports(Ast *root, const char *base_dir) {
    (void)root;
    (void)base_dir;
}

void ll_bytecode_prefetch_wait(const char *resolved_path) {
    (void)resolved_path;
}
