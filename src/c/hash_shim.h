/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_HASH_SHIM_H
#define LL_HASH_SHIM_H

#include <stddef.h>
#include <stdint.h>

/*
 * M8: `builtins.hashString` over OpenSSL's libcrypto, loaded via dlopen/
 * dlsym at first use -- not linked at build time. Same reasoning and the
 * same two-tier candidate-path strategy as `~/gnix/nixgoose`'s
 * `openssl_shim.h`/`blake3_shim.h` (read in full before this file was
 * written): heavy C deps (libcrypto has no business being a hard link-time
 * dependency of every `make` invocation) stay out of the default build
 * graph per docs/architecture.md's "Fast iteration loop" requirement, and
 * get pulled in lazily, only when a program actually calls `hashString`.
 *
 * Which libcrypto API: confirmed directly with `nm -D` against both this
 * machine's system libcrypto (/usr/lib/x86_64-linux-gnu/libcrypto.so.3,
 * OpenSSL 3.6) and the nixpkgs-resolved one the flake's devShell points at
 * (openssl-3.5.9) that the classic one-shot wrappers `MD5()`/`SHA1()`/
 * `SHA256()`/`SHA512()` (signature `unsigned char *(*)(const unsigned
 * char*, size_t, unsigned char*)`) are still exported in both, despite
 * being deprecated at the *header* level in OpenSSL 3.x (a compile-time-
 * only warning that dlsym-based loading never triggers, since we never
 * compile against <openssl/md5.h> etc). This is exactly the same API
 * nixgoose's own openssl_shim.h already uses and exactly what upstream
 * real Nix's libutil/hash.cc itself calls -- no EVP_MD_CTX plumbing is
 * needed since every call site here already has the whole input as one
 * contiguous buffer (never streamed).
 */

#define LL_HASH_MD5_LEN    16
#define LL_HASH_SHA1_LEN   20
#define LL_HASH_SHA256_LEN 32
#define LL_HASH_SHA512_LEN 64

/* Each writes exactly the raw digest (no hex) into `out`, which must be at
 * least the corresponding LL_HASH_*_LEN bytes. Returns 0 on success, -1
 * if libcrypto couldn't be dlopen'd at all (checked LL_LIBCRYPTO_LIB env
 * var first, then a handful of bare-soname/absolute-path fallbacks -- see
 * hash_shim.c), -2 if the specific digest symbol wasn't found in it. */
int64_t ll_hash_md5(const void *input, size_t input_len, unsigned char *out);
int64_t ll_hash_sha1(const void *input, size_t input_len, unsigned char *out);
int64_t ll_hash_sha256(const void *input, size_t input_len, unsigned char *out);
int64_t ll_hash_sha512(const void *input, size_t input_len, unsigned char *out);

/* Lowercase hex-encodes `n` raw digest bytes into `out_hex`, which must be
 * at least 2*n bytes (no null terminator is written -- every caller here
 * already knows the exact length it wants, matching the rest of this
 * codebase's non-null-terminated String convention, see heap.h). */
void ll_hash_to_hex(const unsigned char *bytes, size_t n, char *out_hex);

#endif /* LL_HASH_SHIM_H */
