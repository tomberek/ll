/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "store_path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base32.h"

#define ASNIX_STORE_PATH_MAX_NAME_LEN 211

void asnix_check_store_path_name(const char *name) {
    size_t len = strlen(name);
    if (len == 0) {
        fprintf(stderr, "asnix: invalid derivation name: name must not be empty. Please pass a different 'name'.\n");
        exit(1);
    }
    if (len > ASNIX_STORE_PATH_MAX_NAME_LEN) {
        fprintf(stderr,
                "asnix: invalid derivation name: name '%s' must be no longer than %d characters. "
                "Please pass a different 'name'.\n",
                name, ASNIX_STORE_PATH_MAX_NAME_LEN);
        exit(1);
    }
    if (name[0] == '.') {
        if (len == 1 || (name[1] == '-') || (len >= 2 && name[1] == '.' && (len == 2 || name[2] == '-'))) {
            fprintf(stderr, "asnix: invalid derivation name: name '%s' is not valid. Please pass a different 'name'.\n", name);
            exit(1);
        }
    }
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        int ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 c == '+' || c == '-' || c == '.' || c == '_' || c == '?' || c == '=';
        if (!ok) {
            fprintf(stderr,
                    "asnix: invalid derivation name: name '%s' contains illegal character '%c'. "
                    "Please pass a different 'name'.\n",
                    name, c);
            exit(1);
        }
    }
}

size_t asnix_store_path_preimage(const char *type, const char *hash_with_algo, const char *name, char *out,
                                  size_t out_cap) {
    int n = snprintf(out, out_cap, "%s:%s:%s:%s", type, hash_with_algo, ASNIX_STORE_DIR, name);
    if (n < 0 || (size_t)n >= out_cap) return 0;
    return (size_t)n;
}

static void compress_hash(const uint8_t *digest, size_t digest_len, uint8_t *out, size_t out_len) {
    memset(out, 0, out_len);
    for (size_t i = 0; i < digest_len; i++) {
        out[i % out_len] ^= digest[i];
    }
}

size_t asnix_make_store_path_from_digest(const uint8_t digest[32], const char *name, char *out, size_t out_cap) {
    uint8_t compressed[20];
    compress_hash(digest, 32, compressed, sizeof(compressed));

    char hash32[32]; /* asnix_base32_encoded_length(20) == 32, fixed by the format */
    asnix_base32_encode(compressed, sizeof(compressed), hash32);

    int n = snprintf(out, out_cap, "%s/%.32s-%s", ASNIX_STORE_DIR, hash32, name);
    if (n < 0 || (size_t)n >= out_cap) return 0;
    return (size_t)n;
}
