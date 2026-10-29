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
 * Ported directly from the real implementation at
 * ~/nix/src/libutil/base-nix-32.cc (`BaseNix32::encode`) and verified
 * against real `nix-instantiate`-computed store paths before being used
 * for anything -- see docs/architecture.md's M8 notes.
 */

/* Number of base32 characters needed to encode `n` raw bytes. */
size_t ll_base32_encoded_length(size_t n);

/* Writes exactly ll_base32_encoded_length(len) characters to `out`
 * (NOT null-terminated -- caller sizes/terminates the buffer). */
void ll_base32_encode(const uint8_t *data, size_t len, char *out);

/* M54: the decode half, needed by `builtins.convertHash`'s "nix32"/
 * "base32" input case -- ported directly from real Nix's own
 * `BaseNix32::decode` (~/nix/src/libutil/base-nix-32.cc), which reads
 * characters MOST-significant first (from the end of the string backward)
 * -- the mirror image of ll_base32_encode above. `out_len` must be
 * exactly the expected decoded byte count (known ahead of time by the one
 * caller, convertHash, from the hash algorithm's fixed digest size); `out`
 * is zeroed first since the bit-packing writes with `|=`, not `=`. Returns
 * 0 on success, -1 if `s` contains a character outside the Nix32 alphabet. */
int ll_base32_decode(const char *s, size_t len, uint8_t *out, size_t out_len);

#endif /* LL_BASE32_H */
