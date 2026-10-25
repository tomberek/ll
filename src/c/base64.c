/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "base64.h"

static const char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t asnix_base64_encoded_length(size_t n) {
    return (n + 2) / 3 * 4;
}

void asnix_base64_encode(const uint8_t *data, size_t len, char *out) {
    size_t oi = 0;
    unsigned int acc = 0, nbits = 0;
    for (size_t i = 0; i < len; i++) {
        acc = (acc << 8) | data[i];
        nbits += 8;
        while (nbits >= 6) {
            nbits -= 6;
            out[oi++] = ALPHABET[(acc >> nbits) & 0x3f];
        }
    }
    if (nbits) out[oi++] = ALPHABET[(acc << (6 - nbits)) & 0x3f];
    while (oi % 4) out[oi++] = '=';
}

static int base64_char_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int64_t asnix_base64_decode(const char *s, size_t len, uint8_t *out, size_t out_cap) {
    size_t oi = 0;
    unsigned int acc = 0, nbits = 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '=') break;
        if (c == '\n') continue;
        int digit = base64_char_value(c);
        if (digit < 0) return -1;
        acc = (acc << 6) | (unsigned int)digit;
        nbits += 6;
        if (nbits >= 8) {
            if (oi >= out_cap) return -1;
            out[oi++] = (uint8_t)((acc >> (nbits - 8)) & 0xff);
            nbits -= 8;
        }
    }
    return (int64_t)oi;
}
