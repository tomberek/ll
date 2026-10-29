/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_BASE32_H
#define LL_BASE32_H

#include <stddef.h>
#include <stdint.h>

/*
 * Nix's own base32 variant (NOT RFC4648): alphabet
 * "0123456789abcdfghijklmnpqrsvwxyz" (omits e, o, u, t to avoid spelling
 * words and avoid visual ambiguity), and a non-standard bit-packing order
 * (characters are emitted most-significant-chunk first, but the chunks
 * themselves are read least-significant-bit-first within each byte).
 */

size_t ll_base32_encoded_length(size_t n);

void ll_base32_encode(const uint8_t *data, size_t len, char *out);

/* `out_len` must be exactly the expected decoded byte count; `out` is
 * zeroed first since the bit-packing writes with `|=`, not `=`. */
int ll_base32_decode(const char *s, size_t len, uint8_t *out, size_t out_len);

#endif /* LL_BASE32_H */
