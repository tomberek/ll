/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_BASE64_H
#define LL_BASE64_H

#include <stddef.h>
#include <stdint.h>

/*
 * Standard RFC 4648 base64 (alphabet "A-Za-z0-9+/", '='-padded) -- NOT the
 * same alphabet/packing as base32.h's Nix-specific variant. Needed by M54's
 * `builtins.convertHash` (the "base64"/"sri" hash-format cases) and ported
 * directly from real Nix's own `~/nix/src/libutil/base-n.cc`
 * (`base64::encode`/`base64::decode`), verified against its exact output
 * byte-for-byte via the hash values in numtide/nix-conformance's
 * eval-okay-convertHash.exp before being trusted for anything.
 */

/* Number of base64 characters (including '=' padding) needed to encode `n`
 * raw bytes. */
size_t ll_base64_encoded_length(size_t n);

/* Writes exactly ll_base64_encoded_length(len) characters to `out`
 * (NOT null-terminated), '='-padded to a multiple of 4. */
void ll_base64_encode(const uint8_t *data, size_t len, char *out);

/* Decodes `len` base64 characters from `s` into `out` (caller-sized,
 * capacity `out_cap`), stopping at the first '=' padding character (or the
 * end of input) and skipping embedded newlines -- same permissive behavior
 * as real Nix's own decoder. Returns the number of raw bytes written, or
 * -1 if `s` contains a character outside the base64 alphabet (and not '='
 * or '\n') or the decoded output would exceed `out_cap`. */
int64_t ll_base64_decode(const char *s, size_t len, uint8_t *out, size_t out_cap);

#endif /* LL_BASE64_H */
