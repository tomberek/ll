/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "hash_shim.h"

#include <dlfcn.h>
#include <stdlib.h>

/* M146: the legacy one-shot wrappers (MD5()/SHA1()/SHA256()/SHA512(),
 * what this file used to call exclusively) still work under OpenSSL 3.x,
 * but internally they're now reimplemented as a full EVP_Digest() call --
 * fetch an EVP_MD from the provider registry, init, update, final, free
 * the fetched EVP_MD -- repeated on EVERY call, not just once. Measured in
 * isolation against a from-scratch microbenchmark (docs/architecture.md
 * M146): for d.nix/ffmpeg's real derivation-hashing workload (34,988
 * `asnix_hash_sha256` calls, avg 1649.5 bytes), that provider-fetch
 * machinery costs a ~3,122-instruction FIXED tax per call, independent of
 * input size -- about half of asnix's own total hashing instruction
 * budget, and a cost iets's `sha2` Rust crate (no provider abstraction at
 * all) never pays.
 *
 * The classic streaming triplet -- `MD5_Init`/`_Update`/`_Final` and its
 * SHA1/SHA256/SHA512 siblings -- bypasses the provider-fetch path
 * entirely and computes the exact same digest. Confirmed still exported
 * in both this machine's system libcrypto and the nixpkgs-resolved one
 * (`nm -D`, same discipline M8 used to confirm the one-shot symbols).
 * Byte-identical output is covered by tests/c/test_hash_shim.c's existing
 * oracle values (md5sum/sha1sum/sha256sum/openssl-dgst-transcribed, not
 * re-derived here) -- any mismatch from this change would fail there. */
typedef void *(*asnix_hash_init_fn)(void *ctx);
typedef void *(*asnix_hash_update_fn)(void *ctx, const void *data, size_t len);
typedef void *(*asnix_hash_final_fn)(unsigned char *md, void *ctx);

/* Generously oversized, 8-byte-aligned scratch for OpenSSL's own *_CTX
 * structs -- the largest in play here is SHA512_CTX (8 uint64 state + 2
 * uint64 length counters + a 128-byte block buffer + 2 uint32, ~216 bytes
 * on every LP64 target this runs on). 256 bytes leaves headroom without
 * this file ever needing to include OpenSSL's own headers (see
 * hash_shim.h's header comment on why dlopen/dlsym instead of linking). */
typedef union {
    unsigned char raw[256];
    uint64_t align_marker;
} asnix_hash_ctx;

static void *asnix_libcrypto_handle = 0;

static void *asnix_load_libcrypto(void) {
    if (asnix_libcrypto_handle) return asnix_libcrypto_handle;

    /* Tier 1: an explicit resolved store path, exported by flake.nix's
     * devShell shellHook as ASNIX_LIBCRYPTO_LIB (mirroring nixgoose's
     * NIXGOOSE_LIBBLAKE3_LIB/NIXGOOSE_TOMLC99_LIB convention) -- needed
     * because, unlike a system package manager, nixpkgs' openssl has no
     * single predictable path and isn't on the ambient dlopen search path
     * by soname alone in general. */
    const char *env_path = getenv("ASNIX_LIBCRYPTO_LIB");
    if (env_path) {
        void *h = dlopen(env_path, RTLD_NOW);
        if (h) { asnix_libcrypto_handle = h; return h; }
    }

    /* Tier 2: bare sonames / common absolute paths, tried in case
     * libcrypto happens to be reachable some other way (e.g. this
     * project's own dev machine also has a system libcrypto.so.3 at
     * /usr/lib/x86_64-linux-gnu -- true here, but not assumed to be true
     * on every machine this ever runs on, hence still trying the bare
     * soname first). */
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
        if (h) { asnix_libcrypto_handle = h; return h; }
    }
    return 0;
}

/* perf: `dlsym` does a real symbol-table lookup (hash-bucket walk plus a
 * string compare against every candidate in the bucket, `do_sym`/
 * `_dlerror_run` in a `perf` profile of this file's old behavior) -- cheap
 * once, but this function used to re-resolve all 3 symbols on EVERY single
 * hash call, not just the first. On the real `d.nix`/ffmpeg eval
 * (M146's own count: ~35K `asnix_hash_sha256` calls alone, plus whatever
 * md5/sha1/sha512 call sites add on top), that is 3 redundant `dlsym`
 * calls per hash computed, forever, for a result (the function's address)
 * that can never change mid-process -- `asnix_libcrypto_handle` was
 * already cached this way (see `asnix_load_libcrypto` above); the 3
 * function pointers resolved from it were not, for no reason tied to
 * correctness. One cache slot per algorithm (4 total, each populated via
 * `asnix_hash_one`'s own `cache` out-param on first use, read directly
 * thereafter) -- a `dlsym` per call becomes a `dlsym` per algorithm per
 * process, matching `asnix_load_libcrypto`'s own one-time-resolution
 * discipline instead of re-deriving what's already known. */
typedef struct {
    asnix_hash_init_fn init;
    asnix_hash_update_fn update;
    asnix_hash_final_fn final_fn;
    int resolved; /* 1 once init/update/final_fn have been looked up (even
                   * if any came back NULL -- a symbol genuinely missing
                   * from this libcrypto isn't going to appear later, so a
                   * failed resolution is cached too, not retried forever). */
} AsnixHashFnCache;

static int64_t asnix_hash_one(const char *init_sym, const char *update_sym,
                               const char *final_sym, const void *input,
                               size_t input_len, unsigned char *out,
                               AsnixHashFnCache *cache) {
    if (!cache->resolved) {
        void *h = asnix_load_libcrypto();
        if (!h) return -1;
        cache->init = (asnix_hash_init_fn)dlsym(h, init_sym);
        cache->update = (asnix_hash_update_fn)dlsym(h, update_sym);
        cache->final_fn = (asnix_hash_final_fn)dlsym(h, final_sym);
        cache->resolved = 1;
    }
    if (!cache->init || !cache->update || !cache->final_fn) return -2;
    asnix_hash_ctx ctx;
    cache->init(&ctx);
    cache->update(&ctx, input, input_len);
    cache->final_fn(out, &ctx);
    return 0;
}

int64_t asnix_hash_md5(const void *input, size_t input_len, unsigned char *out) {
    static AsnixHashFnCache cache = { 0 };
    return asnix_hash_one("MD5_Init", "MD5_Update", "MD5_Final", input, input_len, out, &cache);
}
int64_t asnix_hash_sha1(const void *input, size_t input_len, unsigned char *out) {
    static AsnixHashFnCache cache = { 0 };
    return asnix_hash_one("SHA1_Init", "SHA1_Update", "SHA1_Final", input, input_len, out, &cache);
}
int64_t asnix_hash_sha256(const void *input, size_t input_len, unsigned char *out) {
    static AsnixHashFnCache cache = { 0 };
    return asnix_hash_one("SHA256_Init", "SHA256_Update", "SHA256_Final", input, input_len, out, &cache);
}
int64_t asnix_hash_sha512(const void *input, size_t input_len, unsigned char *out) {
    static AsnixHashFnCache cache = { 0 };
    return asnix_hash_one("SHA512_Init", "SHA512_Update", "SHA512_Final", input, input_len, out, &cache);
}

void asnix_hash_to_hex(const unsigned char *bytes, size_t n, char *out_hex) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out_hex[2 * i] = digits[bytes[i] >> 4];
        out_hex[2 * i + 1] = digits[bytes[i] & 0xf];
    }
}
