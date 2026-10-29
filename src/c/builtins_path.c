/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#define _POSIX_C_SOURCE 200809L /* strndup */

#include "builtins_path.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ll/ir.h" /* ll_force_for_print */
#include "base32.h"
#include "builtins_shared.h"
#include "nar.h"
#include "string_context.h"
#include "symtab.h"
#include "value_util.h"

/* Decodes a bare (no algo prefix) sha256 hash string into exactly 32 raw
 * bytes -- either base16 (64 hex chars) or Nix's own base32 (52 chars,
 * base32.h). Narrower than real Nix's `newHashAllowEmpty` (which also
 * accepts base64/SRI). */
static int decode_hex32(const char *hex, size_t len, uint8_t out[32]) {
    if (len == 64) {
        for (int i = 0; i < 32; i++) {
            int hi = hex[2 * i], lo = hex[2 * i + 1];
            int hv = (hi >= '0' && hi <= '9') ? hi - '0' : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10 : -1;
            int lv = (lo >= '0' && lo <= '9') ? lo - '0' : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10 : -1;
            if (hv < 0 || lv < 0) return 0;
            out[i] = (uint8_t)((hv << 4) | lv);
        }
        return 1;
    }
    if (len == ll_base32_encoded_length(32)) {
        return ll_base32_decode(hex, len, out, 32) == 0;
    }
    return 0;
}

/* Closure over the Nix-level `filter` function, threaded through
 * nar.c's plain-C `LlNarFilter` callback shape. */
typedef struct {
    Value fn;
} PathFilterCtx;

static int path_filter_cb(void *ctx_, const char *path, const char *type) {
    PathFilterCtx *ctx = ctx_;
    Value a = ll_make_string_value(path, strlen(path));
    Value b = ll_make_string_value(type, strlen(type));
    Value res = ll_force_for_print(ll_bshared_apply2(ctx->fn, a, b));
    if (ll_tag_of(res) != LL_TAG_IMM
        || (ll_imm_subtag(res) != LL_IMM_TRUE && ll_imm_subtag(res) != LL_IMM_FALSE)) {
        fprintf(stderr, "ll: path filter function must return a boolean\n");
        exit(1);
    }
    return ll_imm_subtag(res) == LL_IMM_TRUE;
}

static char *path_value_to_cstr(Value v, const char *who) {
    v = ll_force_for_print(v);
    int64_t len;
    const char *bytes;
    ll_bshared_string_bytes(v, &bytes, &len, who);
    return strndup(bytes, (size_t)len);
}

/* Shared tail: runs the (possibly filtered/flat/pinned) copy and
 * attaches the resulting path's own Opaque context. */
static Value finish_path_copy(const char *path, const char *name, int recursive, int have_filter, Value filter_fn,
                               int have_sha256, const uint8_t sha256[32]) {
    PathFilterCtx fctx;
    fctx.fn = filter_fn;
    char out_path[300];
    ll_copy_path_to_store_ext(path, name, recursive, have_filter ? path_filter_cb : NULL,
                                  have_filter ? &fctx : NULL, have_sha256 ? sha256 : NULL, out_path,
                                  sizeof(out_path));
    Value out = ll_make_string_value(out_path, strlen(out_path));
    LlStringContext ctx;
    ll_ctx_init(&ctx);
    ll_ctx_add_opaque(&ctx, out_path);
    ll_string_attach_context(out, &ctx);
    return out;
}

static Value prim_path(const Value *args, int64_t argc) {
    (void)argc;
    LL_REQUIRE_ATTRS(args[0], "path");

    char *path_str = NULL;
    char *name = NULL;
    int have_filter = 0;
    Value filter_fn = LL_NULL;
    int recursive = 1;
    int have_sha256 = 0;
    uint8_t sha256[32];

    int64_t n = ll_bshared_attrs_count(args[0]);
    for (int64_t i = 0; i < n; i++) {
        uint32_t sym = ll_bshared_attrs_sym(args[0], i);
        size_t klen;
        const char *kname = ll_sym_name(sym, &klen);
        Value val = ll_bshared_attrs_value(args[0], i);
        if (klen == 4 && memcmp(kname, "path", 4) == 0) {
            path_str = path_value_to_cstr(val, "path");
        } else if (klen == 4 && memcmp(kname, "name", 4) == 0) {
            name = path_value_to_cstr(val, "path");
        } else if (klen == 6 && memcmp(kname, "filter", 6) == 0) {
            filter_fn = ll_force_for_print(val);
            have_filter = 1;
        } else if (klen == 9 && memcmp(kname, "recursive", 9) == 0) {
            Value rv = ll_force_for_print(val);
            if (ll_tag_of(rv) != LL_TAG_IMM
                || (ll_imm_subtag(rv) != LL_IMM_TRUE && ll_imm_subtag(rv) != LL_IMM_FALSE)) {
                fprintf(stderr, "ll: path: 'recursive' must be a boolean\n");
                exit(1);
            }
            recursive = ll_imm_subtag(rv) == LL_IMM_TRUE;
        } else if (klen == 6 && memcmp(kname, "sha256", 6) == 0) {
            Value sv = ll_force_for_print(val);
            int64_t slen;
            const char *sbytes;
            ll_bshared_string_bytes(sv, &sbytes, &slen, "path");
            if (!decode_hex32(sbytes, (size_t)slen, sha256)) {
                fprintf(stderr, "ll: path: 'sha256' must be a 64-character hex digest or a 52-character nix32 one\n");
                exit(1);
            }
            have_sha256 = 1;
        } else {
            fprintf(stderr, "ll: unsupported argument '%.*s' to 'builtins.path'\n", (int)klen, kname);
            exit(1);
        }
    }
    if (!path_str) {
        fprintf(stderr, "ll: missing required 'path' attribute in the first argument to 'builtins.path'\n");
        exit(1);
    }

    Value result = finish_path_copy(path_str, name, recursive, have_filter, filter_fn, have_sha256, sha256);
    free(path_str);
    free(name);
    return result;
}
static Value build_path(void) { return ll_bshared_make_primop((void *)prim_path, 1); }

static Value filterSource2_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value filter_fn = ll_force_for_print(args[0]);
    char *path_str = path_value_to_cstr(args[1], "filterSource");
    Value result = finish_path_copy(path_str, NULL, 1, 1, filter_fn, 0, NULL);
    free(path_str);
    return result;
}
static Value build_filterSource(void) { return ll_bshared_make_curried_native((void *)filterSource2_impl, 2); }

const LlBuiltinMiscDef LL_PATH_BUILTINS[] = {
    { "path", build_path },
    { "filterSource", build_filterSource },
};
const size_t LL_PATH_BUILTINS_COUNT = sizeof(LL_PATH_BUILTINS) / sizeof(LL_PATH_BUILTINS[0]);
