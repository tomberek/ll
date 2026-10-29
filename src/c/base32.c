/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "base32.h"

#include <string.h>

static const char ALPHABET[] = "0123456789abcdfghijklmnpqrsvwxyz";

static int base32_char_value(char c) {
    for (int i = 0; i < 32; i++) {
        if (ALPHABET[i] == c) return i;
    }
    return -1;
}

size_t ll_base32_encoded_length(size_t n) {
    if (n == 0) return 0;
    return (n * 8 - 1) / 5 + 1;
}

void ll_base32_encode(const uint8_t *data, size_t len, char *out) {
    if (len == 0) return;
    size_t enc_len = ll_base32_encoded_length(len);
    for (size_t idx = 0; idx < enc_len; idx++) {
        size_t n = enc_len - 1 - idx;
        unsigned int b = (unsigned int)(n * 5);
        unsigned int i = b / 8;
        unsigned int j = b % 8;
        unsigned int c = data[i] >> j;
        if (i < len - 1) {
            c |= (unsigned int)(data[i + 1] << (8 - j));
        }
        out[idx] = ALPHABET[c & 0x1f];
    }
}

int ll_base32_decode(const char *s, size_t len, uint8_t *out, size_t out_len) {
    memset(out, 0, out_len);
    for (size_t n = 0; n < len; n++) {
        char c = s[len - n - 1];
        int digit = base32_char_value(c);
        if (digit < 0) return -1;
        size_t b = n * 5;
        size_t i = b / 8;
        size_t j = b % 8;
        if (i < out_len) out[i] |= (uint8_t)(digit << j);
        if ((digit >> (8 - j)) && i + 1 < out_len) out[i + 1] |= (uint8_t)(digit >> (8 - j));
    }
    return 0;
}
