/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_BASE64_H
#define LL_BASE64_H

#include <stddef.h>
#include <stdint.h>

size_t ll_base64_encoded_length(size_t n);

void ll_base64_encode(const uint8_t *data, size_t len, char *out);

int64_t ll_base64_decode(const char *s, size_t len, uint8_t *out, size_t out_cap);

#endif /* LL_BASE64_H */
