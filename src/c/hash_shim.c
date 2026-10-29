/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "hash_shim.h"

#include <dlfcn.h>
#include <stdlib.h>

/* Uses the classic streaming triplet -- `MD5_Init`/`_Update`/`_Final`
 * and its SHA1/SHA256/SHA512 siblings -- rather than the one-shot
 * wrappers; both compute the same digest. Byte-identical output is
 * covered by tests/c/test_hash_shim.c's oracle values. */
typedef void *(*ll_hash_init_fn)(void *ctx);
typedef void *(*ll_hash_update_fn)(void *ctx, const void *data, size_t len);
typedef void *(*ll_hash_final_fn)(unsigned char *md, void *ctx);

/* Generously oversized, 8-byte-aligned scratch for OpenSSL's own *_CTX
 * structs -- the largest in play here is SHA512_CTX (8 uint64 state + 2
 * uint64 length counters + a 128-byte block buffer + 2 uint32, ~216 bytes
 * on every LP64 target this runs on). 256 bytes leaves headroom without
 * this file ever needing to include OpenSSL's own headers. */
typedef union {
    unsigned char raw[256];
    uint64_t align_marker;
} ll_hash_ctx;

static void *ll_libcrypto_handle = 0;

static void *ll_load_libcrypto(void) {
    if (ll_libcrypto_handle) return ll_libcrypto_handle;

    /* Tier 1: an explicit resolved store path, exported by flake.nix's
     * devShell shellHook as LL_LIBCRYPTO_LIB -- needed because, unlike a
     * system package manager, nixpkgs' openssl has no single predictable
     * path and isn't on the ambient dlopen search path by soname alone
     * in general. */
    const char *env_path = getenv("LL_LIBCRYPTO_LIB");
    if (env_path) {
        void *h = dlopen(env_path, RTLD_NOW);
        if (h) { ll_libcrypto_handle = h; return h; }
    }

    /* Tier 2: bare sonames / common absolute paths, tried in case
     * libcrypto happens to be reachable some other way. */
    static const char *candidates[] = {
        "libcrypto.so.3",
        "libcrypto.so",
        "/usr/lib/x86_64-linux-gnu/libcrypto.so.3",
        "/usr/lib64/libcrypto.so.3",
        "/usr/lib/libcrypto.so.3",
        "/lib/x86_64-linux-gnu/libcrypto.so.3",
        "/usr/local/lib/libcrypto.so.3",
        0,
    };
    for (int i = 0; candidates[i]; i++) {
        void *h = dlopen(candidates[i], RTLD_NOW);
        if (h) { ll_libcrypto_handle = h; return h; }
    }
    return 0;
}

/* One cache slot per algorithm (4 total), each populated via
 * `ll_hash_one`'s own `cache` out-param on first use, read directly
 * thereafter, so `dlsym` only runs once per algorithm per process. */
typedef struct {
    ll_hash_init_fn init;
    ll_hash_update_fn update;
    ll_hash_final_fn final_fn;
    int resolved; /* 1 once init/update/final_fn have been looked up (even
                   * if any came back NULL -- a failed resolution is
                   * cached too, not retried forever). */
} LlHashFnCache;

static int64_t ll_hash_one(const char *init_sym, const char *update_sym,
                               const char *final_sym, const void *input,
                               size_t input_len, unsigned char *out,
                               LlHashFnCache *cache) {
    if (!cache->resolved) {
        void *h = ll_load_libcrypto();
        if (!h) return -1;
        cache->init = (ll_hash_init_fn)dlsym(h, init_sym);
        cache->update = (ll_hash_update_fn)dlsym(h, update_sym);
        cache->final_fn = (ll_hash_final_fn)dlsym(h, final_sym);
        cache->resolved = 1;
    }
    if (!cache->init || !cache->update || !cache->final_fn) return -2;
    ll_hash_ctx ctx;
    cache->init(&ctx);
    cache->update(&ctx, input, input_len);
    cache->final_fn(out, &ctx);
    return 0;
}

int64_t ll_hash_md5(const void *input, size_t input_len, unsigned char *out) {
    static LlHashFnCache cache = { 0 };
    return ll_hash_one("MD5_Init", "MD5_Update", "MD5_Final", input, input_len, out, &cache);
}
int64_t ll_hash_sha1(const void *input, size_t input_len, unsigned char *out) {
    static LlHashFnCache cache = { 0 };
    return ll_hash_one("SHA1_Init", "SHA1_Update", "SHA1_Final", input, input_len, out, &cache);
}
int64_t ll_hash_sha256(const void *input, size_t input_len, unsigned char *out) {
    static LlHashFnCache cache = { 0 };
    return ll_hash_one("SHA256_Init", "SHA256_Update", "SHA256_Final", input, input_len, out, &cache);
}
int64_t ll_hash_sha512(const void *input, size_t input_len, unsigned char *out) {
    static LlHashFnCache cache = { 0 };
    return ll_hash_one("SHA512_Init", "SHA512_Update", "SHA512_Final", input, input_len, out, &cache);
}

void ll_hash_to_hex(const unsigned char *bytes, size_t n, char *out_hex) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out_hex[2 * i] = digits[bytes[i] >> 4];
        out_hex[2 * i + 1] = digits[bytes[i] & 0xf];
    }
}
