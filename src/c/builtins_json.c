/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "builtins_json.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>

#include "ll/value.h"
#include "builtins_shared.h"
#include "symtab.h"
#include "value_util.h"

typedef struct {
    const char *src;
    size_t len;
    size_t i;
    /* Selects the integer-overflow policy: `builtins.fromJSON` must
     * error on an integer literal too big for Nix's Int type, while
     * derivation.c's `__json` attribute parsing must instead silently
     * demote it to a float (matching the two different real-Nix code
     * paths each one goes through) -- the two entry points below set
     * this differently rather than sharing one policy. */
    int permissive_overflow;
} JsonParser;

static void json_fatal(const char *msg) {
    fprintf(stderr, "ll: fromJSON: %s\n", msg);
    exit(1);
}

static char jp_cur(JsonParser *p) {
    return p->i < p->len ? p->src[p->i] : '\0';
}

static void jp_skip_ws(JsonParser *p) {
    while (p->i < p->len) {
        char c = p->src[p->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->i++;
        else break;
    }
}

static void jp_expect(JsonParser *p, char c) {
    if (jp_cur(p) != c) json_fatal("malformed JSON (unexpected character)");
    p->i++;
}

/* Growable byte buffer, same shape as lexer.c's ByteBuf (re-declared
 * here rather than shared). */
typedef struct {
    char *buf;
    size_t len, cap;
} ByteBuf;

static void bb_ensure(ByteBuf *bb, size_t extra) {
    if (bb->len + extra <= bb->cap) return;
    size_t ncap = bb->cap ? bb->cap * 2 : 32;
    while (ncap < bb->len + extra) ncap *= 2;
    bb->buf = realloc(bb->buf, ncap);
    bb->cap = ncap;
}

static void bb_push(ByteBuf *bb, char c) {
    bb_ensure(bb, 1);
    bb->buf[bb->len++] = c;
}

static int hex_digit_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    json_fatal("invalid hex digit in \\u escape");
    return 0;
}

static uint32_t parse_hex4(JsonParser *p) {
    if (p->i + 4 > p->len) json_fatal("truncated \\u escape");
    uint32_t v = 0;
    for (int k = 0; k < 4; k++) v = (v << 4) | (uint32_t)hex_digit_value(p->src[p->i + (size_t)k]);
    p->i += 4;
    return v;
}

/* Standard UTF-8 encoding of a Unicode code point -- returns the byte
 * count written (1..4). */
static size_t utf8_encode(uint32_t cp, char *out) {
    if (cp <= 0x7F) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Parses a JSON string, `p->i` already positioned just past the opening
 * '"'. Decodes \" \\ \/ \b \f \n \r \t directly and \uXXXX via
 * utf8_encode, including the UTF-16 surrogate-pair case (a high
 * surrogate 0xD800-0xDBFF must be immediately followed by a low
 * surrogate 0xDC00-0xDFFF, combined per the standard formula into one
 * codepoint above U+FFFF -- an unpaired surrogate is rejected rather
 * than silently mis-encoded). */
static Value parse_json_string(JsonParser *p) {
    ByteBuf bb = { 0 };
    for (;;) {
        if (p->i >= p->len) json_fatal("unterminated string");
        char c = p->src[p->i];
        if (c == '"') {
            p->i++;
            break;
        }
        if ((unsigned char)c < 0x20) json_fatal("control character in string");
        if (c != '\\') {
            bb_push(&bb, c);
            p->i++;
            continue;
        }
        p->i++;
        if (p->i >= p->len) json_fatal("unterminated escape");
        char e = p->src[p->i++];
        switch (e) {
            case '"': bb_push(&bb, '"'); break;
            case '\\': bb_push(&bb, '\\'); break;
            case '/': bb_push(&bb, '/'); break;
            case 'b': bb_push(&bb, '\b'); break;
            case 'f': bb_push(&bb, '\f'); break;
            case 'n': bb_push(&bb, '\n'); break;
            case 'r': bb_push(&bb, '\r'); break;
            case 't': bb_push(&bb, '\t'); break;
            case 'u': {
                uint32_t cp = parse_hex4(p);
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (p->i + 2 > p->len || p->src[p->i] != '\\' || p->src[p->i + 1] != 'u') {
                        json_fatal("unpaired UTF-16 surrogate in \\u escape");
                    }
                    p->i += 2;
                    uint32_t lo = parse_hex4(p);
                    if (lo < 0xDC00 || lo > 0xDFFF) json_fatal("invalid UTF-16 surrogate pair");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    json_fatal("unpaired UTF-16 surrogate in \\u escape");
                }
                char utf8[4];
                size_t n = utf8_encode(cp, utf8);
                for (size_t k = 0; k < n; k++) bb_push(&bb, utf8[k]);
                break;
            }
            default: json_fatal("invalid escape sequence");
        }
    }
    /* A JSON string's \u0000 escape (or a literal NUL byte) can produce
     * an embedded NUL -- real Nix rejects this outright for any string
     * value it builds this way, for both JSON string VALUES and object
     * KEYS. asnix's own String representation tracks length explicitly
     * rather than relying on NUL-termination, so it would otherwise
     * silently accept this: do not drop this check. Checked once here,
     * in the one shared string-decoding function both keys and values
     * go through. */
    for (size_t k = 0; k < bb.len; k++) {
        if (bb.buf[k] == '\0') json_fatal("input string cannot be represented as Nix string because it contains null bytes");
    }
    Value out = ll_make_string_value(bb.buf ? bb.buf : "", bb.len);
    free(bb.buf);
    return out;
}

/* JSON number grammar: `-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?`.
 * A '.' or exponent present makes this a Float Value (strtod); otherwise
 * it's an Int Value (strtoll) -- matching real Nix's own fromJSON int/
 * float split. */
static Value parse_json_number(JsonParser *p) {
    size_t start = p->i;
    if (jp_cur(p) == '-') p->i++;
    if (!isdigit((unsigned char)jp_cur(p))) json_fatal("invalid number");
    while (isdigit((unsigned char)jp_cur(p))) p->i++;
    int is_float = 0;
    if (jp_cur(p) == '.') {
        is_float = 1;
        p->i++;
        if (!isdigit((unsigned char)jp_cur(p))) json_fatal("invalid number (digits required after '.')");
        while (isdigit((unsigned char)jp_cur(p))) p->i++;
    }
    if (jp_cur(p) == 'e' || jp_cur(p) == 'E') {
        is_float = 1;
        p->i++;
        if (jp_cur(p) == '+' || jp_cur(p) == '-') p->i++;
        if (!isdigit((unsigned char)jp_cur(p))) json_fatal("invalid number (digits required in exponent)");
        while (isdigit((unsigned char)jp_cur(p))) p->i++;
    }
    size_t len = p->i - start;
    char *buf = malloc(len + 1);
    memcpy(buf, p->src + start, len);
    buf[len] = '\0';
    Value out;
    if (is_float) {
        out = ll_make_float_value(strtod(buf, NULL));
    } else {
        /* An integer JSON number that overflows even a real int64_t must
         * fail loudly, not silently wrap or saturate. `errno` alone only
         * catches strtoll's own ERANGE (past INT64_MIN/MAX) -- the
         * explicit LL_INT_MIN/MAX bounds check is also required: it
         * catches the narrower band asnix itself actually supports (a
         * value that fits in a real int64_t but not asnix's 60-bit one
         * would otherwise silently truncate when shifted into the tagged
         * word). Do not drop either check. */
        errno = 0;
        long long val = strtoll(buf, NULL, 10);
        if (errno == ERANGE || val < LL_INT_MIN || val > LL_INT_MAX) {
            /* `__json`'s own parser (see `permissive_overflow` above)
             * demotes to a float here instead of erroring. */
            if (p->permissive_overflow) {
                out = ll_make_float_value(strtod(buf, NULL));
                free(buf);
                return out;
            }
            json_fatal("number outside of Nix integer range");
        }
        out = ll_make_int((int64_t)val);
    }
    free(buf);
    return out;
}

static Value parse_json_value(JsonParser *p);

static Value parse_json_array(JsonParser *p) {
    jp_expect(p, '[');
    jp_skip_ws(p);
    size_t cap = 4, n = 0;
    Value *items = malloc(cap * sizeof(Value));
    if (jp_cur(p) == ']') {
        p->i++;
        Value out = ll_bshared_make_list(items, 0);
        free(items);
        return out;
    }
    for (;;) {
        jp_skip_ws(p);
        if (n == cap) {
            cap *= 2;
            items = realloc(items, cap * sizeof(Value));
        }
        items[n++] = parse_json_value(p);
        jp_skip_ws(p);
        char c = jp_cur(p);
        if (c == ',') {
            p->i++;
            continue;
        }
        if (c == ']') {
            p->i++;
            break;
        }
        json_fatal("expected ',' or ']' in array");
    }
    Value out = ll_bshared_make_list(items, (int64_t)n);
    free(items);
    return out;
}

static Value parse_json_object(JsonParser *p) {
    jp_expect(p, '{');
    jp_skip_ws(p);
    size_t cap = 4, n = 0;
    uint32_t *syms = malloc(cap * sizeof(uint32_t));
    Value *vals = malloc(cap * sizeof(Value));
    if (jp_cur(p) == '}') {
        p->i++;
        Value out = ll_bshared_make_attrs(syms, vals, 0);
        free(syms);
        free(vals);
        return out;
    }
    for (;;) {
        jp_skip_ws(p);
        if (jp_cur(p) != '"') json_fatal("expected a string key in object");
        p->i++;
        Value key = parse_json_string(p);
        int64_t klen;
        const char *kbytes = ll_ptr_bytes(key, &klen);
        uint32_t sym = ll_intern(kbytes, (size_t)klen);
        jp_skip_ws(p);
        jp_expect(p, ':');
        jp_skip_ws(p);
        Value val = parse_json_value(p);
        /* A repeated key overwrites the EARLIER slot's value in place
         * (last-wins) -- do not unconditionally append a repeated key as
         * its own separate slot: every `LL_TAG_ATTRS` consumer in this
         * codebase relies on no duplicate syms existing in one attrset
         * once it reaches `ll_bshared_make_attrs` below. */
        int64_t existing = -1;
        for (int64_t j = 0; j < (int64_t)n; j++) {
            if (syms[j] == sym) { existing = j; break; }
        }
        if (existing >= 0) {
            vals[existing] = val;
        } else {
            if (n == cap) {
                cap *= 2;
                syms = realloc(syms, cap * sizeof(uint32_t));
                vals = realloc(vals, cap * sizeof(Value));
            }
            syms[n] = sym;
            vals[n] = val;
            n++;
        }
        jp_skip_ws(p);
        char c = jp_cur(p);
        if (c == ',') {
            p->i++;
            continue;
        }
        if (c == '}') {
            p->i++;
            break;
        }
        json_fatal("expected ',' or '}' in object");
    }
    Value out = ll_bshared_make_attrs(syms, vals, (int64_t)n);
    free(syms);
    free(vals);
    return out;
}

static Value parse_json_value(JsonParser *p) {
    jp_skip_ws(p);
    char c = jp_cur(p);
    if (c == '{') return parse_json_object(p);
    if (c == '[') return parse_json_array(p);
    if (c == '"') {
        p->i++;
        return parse_json_string(p);
    }
    if (c == 't') {
        if (p->i + 4 > p->len || memcmp(p->src + p->i, "true", 4) != 0) json_fatal("invalid literal");
        p->i += 4;
        return ll_make_imm(LL_IMM_TRUE);
    }
    if (c == 'f') {
        if (p->i + 5 > p->len || memcmp(p->src + p->i, "false", 5) != 0) json_fatal("invalid literal");
        p->i += 5;
        return ll_make_imm(LL_IMM_FALSE);
    }
    if (c == 'n') {
        if (p->i + 4 > p->len || memcmp(p->src + p->i, "null", 4) != 0) json_fatal("invalid literal");
        p->i += 4;
        return ll_make_imm(LL_IMM_NULL);
    }
    if (c == '-' || isdigit((unsigned char)c)) return parse_json_number(p);
    json_fatal("unexpected character");
    return ll_make_imm(LL_IMM_NULL); /* unreachable -- json_fatal exits */
}

static Value ll_json_parse(const char *src, size_t len, int permissive_overflow) {
    JsonParser p = { src, len, 0, permissive_overflow };
    Value out = parse_json_value(&p);
    jp_skip_ws(&p);
    if (p.i != p.len) json_fatal("trailing data after JSON value");
    return out;
}

/* This parser uses one native C stack frame per nesting level, which can
 * blow the default thread stack on a deeply nested literal. Run it on a
 * dedicated, bigger stack via a `ucontext.h` side stack (no actual
 * concurrency; the caller is suspended for the `swapcontext` duration)
 * rather than fixing this with a global `setrlimit(RLIMIT_STACK, ...)`
 * at startup -- that also deepens `builtins.toJSON`'s own, unrelated
 * recursive serializer enough to let `eval-fail-toJSON-stack-overflow.nix`
 * finish instead of crashing, which that conformance case relies on NOT
 * happening. Keep the bigger stack scoped to only this parser's own
 * recursion. */
#define LL_JSON_PARSE_STACK_BYTES (64 * 1024 * 1024)

typedef struct {
    const char *src;
    size_t len;
    int permissive_overflow;
    Value result;
} JsonParseCtx;

/* Single global, not re-entrancy-safe for concurrent use -- fine here
 * since `swapcontext` below runs this to completion before the caller
 * resumes, and any nested `fromJSON` call gets its own fresh
 * JsonParseCtx/stack further down this same call chain. */
static JsonParseCtx *g_json_parse_ctx;

static void json_parse_trampoline(void) {
    g_json_parse_ctx->result =
        ll_json_parse(g_json_parse_ctx->src, g_json_parse_ctx->len, g_json_parse_ctx->permissive_overflow);
}

static Value run_json_parse_on_big_stack(const char *src, size_t len, int permissive_overflow) {
    JsonParseCtx ctx = { src, len, permissive_overflow, ll_make_imm(LL_IMM_NULL) };
    JsonParseCtx *saved_ctx = g_json_parse_ctx; /* restore on return, supporting nested fromJSON calls */
    g_json_parse_ctx = &ctx;

    void *stack_mem = malloc(LL_JSON_PARSE_STACK_BYTES);
    ucontext_t caller_ctx, parse_ctx;
    getcontext(&parse_ctx);
    parse_ctx.uc_stack.ss_sp = stack_mem;
    parse_ctx.uc_stack.ss_size = LL_JSON_PARSE_STACK_BYTES;
    parse_ctx.uc_link = &caller_ctx;
    makecontext(&parse_ctx, json_parse_trampoline, 0);
    swapcontext(&caller_ctx, &parse_ctx);
    free(stack_mem);

    g_json_parse_ctx = saved_ctx;
    return ctx.result;
}

static Value prim_fromJSON(const Value *args, int64_t argc) {
    (void)argc;
    const char *bytes;
    int64_t len;
    ll_bshared_string_bytes(args[0], &bytes, &len, "fromJSON");
    return run_json_parse_on_big_stack(bytes, (size_t)len, /* permissive_overflow */ 0);
}

static Value build_fromJSON(void) { return ll_bshared_make_primop((void *)prim_fromJSON, 1); }

const LlBuiltinMiscDef LL_JSON_BUILTINS[] = {
    { "fromJSON", build_fromJSON },
};

/* Entry point for derivation.c's own `__json` attribute handling -- see
 * `permissive_overflow` (top of `JsonParser`) for why this needs a parse
 * policy distinct from `builtins.fromJSON`'s `prim_fromJSON` above.
 * Fatal (exit 1) on any malformed JSON, same as fromJSON. */
Value ll_json_parse_structured_attrs(const char *bytes, int64_t len) {
    return run_json_parse_on_big_stack(bytes, (size_t)len, /* permissive_overflow */ 1);
}

const size_t LL_JSON_BUILTINS_COUNT = sizeof(LL_JSON_BUILTINS) / sizeof(LL_JSON_BUILTINS[0]);
