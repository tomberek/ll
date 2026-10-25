/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
/*
 * builtins.fromJSON -- M51. A scoped, hand-written recursive-descent JSON
 * parser (RFC 8259), modeled on this project's existing toml.c/lexer.c
 * style: plain index-into-buffer scanning, no external dependency, fatal
 * on malformed input.
 *
 * Previously this builtin didn't exist in this codebase AT ALL (flagged
 * explicitly in docs/architecture.md's long-tail-pass notes: "`fromJSON`
 * doesn't exist in this codebase at all yet... not worth building a JSON
 * parser for in isolation" for the sake of a single conformance file).
 * That calculus changes here: numtide/nix-conformance's
 * eval-okay-fromjson.nix is one of the files this milestone's float
 * support was specifically asked to unblock (it contains a handful of
 * ordinary Nix float literals in the comparison value on its RHS, which
 * is what kept it a parse failure before this milestone) -- but getting
 * past that parse error reveals `fromJSON` is ALSO needed to actually
 * evaluate the file, since it calls `builtins.fromJSON` on a literal
 * JSON document and compares the result against a literal Nix value.
 * Writing a real (if deliberately small) JSON parser is the only honest
 * way to pass that file, so it's in scope here rather than out of scope
 * the way it was before.
 *
 * Implemented: objects, arrays, strings (\" \\ \/ \b \f \n \r \t \uXXXX
 * escapes, including UTF-16 surrogate pairs), numbers (JSON's number
 * grammar: optional leading '-', an int part, an optional '.'-fraction,
 * an optional [eE][+-]?digits exponent -- a '.' or exponent makes it a
 * Float Value, otherwise an Int, matching the same int/float split real
 * Nix's own fromJSON makes, confirmed directly against
 * eval-okay-fromjson.nix's own expected output: `23493`/`116`/`-10`
 * stay plain ints, `3.14159` becomes a float), true/false/null.
 *
 * NOT implemented: JSON5-isms (comments, trailing commas, unquoted
 * keys), duplicate-key "last wins" vs "error" policy choice (this parser
 * does last-wins via plain attrset-overwrite semantics, falling out for
 * free from how the entries array is built -- not verified against real
 * Nix's own choice here since no corpus case exercises it, same "not
 * pursued speculatively" convention as every other untested edge in this
 * codebase). */
#include "builtins_json.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h> /* M67: run_json_parse_on_big_stack -- see its own comment */

#include "asnix/value.h"
#include "builtins_shared.h"
#include "symtab.h"
#include "value_util.h"

typedef struct {
    const char *src;
    size_t len;
    size_t i;
    /* M80: derivation.c's own `__json` attribute (a raw JSON string the
     * user hands `derivation {...}` directly -- re-validated/normalized
     * rather than built from an attrset, see that call site's own
     * comment) needs a DIFFERENT integer-overflow policy than
     * `builtins.fromJSON` below: real Nix's `StructuredAttrs::parse`
     * goes through plain `nlohmann::json::parse` (a DOM parse, no extra
     * validation layered on top), which silently demotes ANY integer
     * literal too big to fit into a float rather than erroring --
     * confirmed directly: `derivation { ... __json =
     * "{\"d\":100000000000000000000}"; }`'s own `.drv` ends up with
     * `"d":1e+20`, not an error, via real `nix-instantiate`.
     * `builtins.fromJSON` goes through a SEPARATE hand-rolled SAX
     * visitor (real Nix's `json-to-value.cc`) that adds its own
     * explicit `number_unsigned` overflow check specifically to keep
     * oversized integers out of the Nix language's own Int type
     * (eval-fail-fromJSON-overflowing.nix depends on that **erroring**,
     * not silently becoming a float) -- so the two entry points
     * genuinely disagree on this, and `permissive_overflow` is this
     * parser's single switch between them, set per entry point below
     * rather than threading an extra argument through every mutually
     * recursive parse_json_* function. */
    int permissive_overflow;
} JsonParser;

static void json_fatal(const char *msg) {
    fprintf(stderr, "asnix: fromJSON: %s\n", msg);
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

/* Growable byte buffer, same shape as lexer.c's ByteBuf -- re-declared
 * here rather than shared, same "no textual coupling between unrelated
 * modules" convention this project already uses (see builtins_misc.c's
 * own header comment on this). */
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
 * count written (1..4). Ported from the same algorithm toml.c's own
 * utf8_encode uses (static to that file, so reimplemented here rather
 * than shared -- see this file's header comment). */
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
 * than silently mis-encoded). Returns an asnix String Value. */
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
    /* A JSON string's \u0000 escape (or, in principle, a literal NUL
     * byte) can produce an embedded NUL -- real Nix rejects this outright
     * for any string value it builds this way (confirmed directly
     * against `nix-instantiate --eval`: `builtins.fromJSON ''"a\u0000b"''`
     * errors "input string 'a\0b' cannot be represented as Nix string
     * because it contains null bytes", for both JSON string VALUES and
     * object KEYS -- found via numtide/nix-conformance's
     * eval-fail-fromJSON-keyWithNullByte.nix/-valueWithNullByte.nix,
     * both of which this parser silently accepted before this check was
     * added, since asnix's own String representation tracks length
     * explicitly rather than relying on NUL-termination and so has no
     * structural reason to reject this on its own). Checked once here,
     * in the one shared string-decoding function both keys and values
     * go through, rather than at each of parse_json_object/
     * parse_json_value's two call sites. */
    for (size_t k = 0; k < bb.len; k++) {
        if (bb.buf[k] == '\0') json_fatal("input string cannot be represented as Nix string because it contains null bytes");
    }
    Value out = asnix_make_string_value(bb.buf ? bb.buf : "", bb.len);
    free(bb.buf);
    return out;
}

/* JSON number grammar: `-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?`.
 * A '.' or exponent present makes this a Float Value (strtod); otherwise
 * it's an Int Value (strtoll) -- matching real Nix's own fromJSON int/
 * float split (confirmed against eval-okay-fromjson.nix's own expected
 * output: `23493`/`116`/`-10` stay ints, `3.14159` becomes a float). */
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
        out = asnix_make_float_value(strtod(buf, NULL));
    } else {
        /* An integer JSON number that overflows even a real int64_t (let
         * alone asnix's own narrower 60-bit range, ASNIX_INT_MIN/MAX --
         * see include/asnix/value.h) must fail loudly, not silently wrap
         * or saturate -- found via numtide/nix-conformance's
         * eval-fail-fromJSON-overflowing.nix (`{"attr":
         * 18446744073709551615}`, 2^64-1, which doesn't fit in ANY
         * signed 64-bit integer), checked against `nix-instantiate
         * --eval`: "unsigned json number 18446744073709551615 outside
         * of Nix integer range". `errno` catches strtoll's own
         * ERANGE (set for anything past INT64_MIN/MAX); the explicit
         * ASNIX_INT_MIN/MAX bounds check on top of that catches the
         * narrower band asnix itself actually supports (a value that
         * fits in a real int64_t but not asnix's 60-bit one would
         * otherwise silently truncate when shifted into the tagged
         * word, same class of bug this check exists to prevent). */
        errno = 0;
        long long val = strtoll(buf, NULL, 10);
        if (errno == ERANGE || val < ASNIX_INT_MIN || val > ASNIX_INT_MAX) {
            /* M80: `__json`'s own parser (see `permissive_overflow`'s
             * header comment above) demotes to a float here instead of
             * erroring -- matching real Nix's plain `nlohmann::json::
             * parse` DOM path used by `StructuredAttrs::parse`, as
             * opposed to `builtins.fromJSON`'s stricter SAX visitor. */
            if (p->permissive_overflow) {
                out = asnix_make_float_value(strtod(buf, NULL));
                free(buf);
                return out;
            }
            json_fatal("number outside of Nix integer range");
        }
        out = asnix_make_int((int64_t)val);
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
        Value out = asnix_bshared_make_list(items, 0);
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
    Value out = asnix_bshared_make_list(items, (int64_t)n);
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
        Value out = asnix_bshared_make_attrs(syms, vals, 0);
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
        const char *kbytes = asnix_ptr_bytes(key, &klen);
        uint32_t sym = asnix_intern(kbytes, (size_t)klen);
        jp_skip_ws(p);
        jp_expect(p, ':');
        jp_skip_ws(p);
        Value val = parse_json_value(p);
        /* M80: a repeated key overwrites the EARLIER slot's value in
         * place -- found chasing eval-okay-json-attr-normalised.nix's
         * `{"a":1,"a":2}` case through to its `.drv` (whose `__json` env
         * entry must come out byte-identical to `{"a":2}`'s own, per
         * real `nix-instantiate`), which this parser's own header
         * comment already claims ("last-wins via plain attrset-
         * overwrite semantics") but the code actually below it didn't
         * implement -- every prior key, even a repeated one, was being
         * unconditionally appended as its own separate slot, violating
         * the invariant every other `ASNIX_TAG_ATTRS` producer in this
         * codebase relies on (no duplicate syms in one attrset) once
         * this array reached `asnix_bshared_make_attrs` below. Matches
         * real Nix's own `ValueMap::insert_or_assign` in its SAX
         * visitor's `JSONObjectState::key()` (json-to-value.cc) --
         * last-wins, one slot per unique key. */
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
    Value out = asnix_bshared_make_attrs(syms, vals, (int64_t)n);
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
        return asnix_make_imm(ASNIX_IMM_TRUE);
    }
    if (c == 'f') {
        if (p->i + 5 > p->len || memcmp(p->src + p->i, "false", 5) != 0) json_fatal("invalid literal");
        p->i += 5;
        return asnix_make_imm(ASNIX_IMM_FALSE);
    }
    if (c == 'n') {
        if (p->i + 4 > p->len || memcmp(p->src + p->i, "null", 4) != 0) json_fatal("invalid literal");
        p->i += 4;
        return asnix_make_imm(ASNIX_IMM_NULL);
    }
    if (c == '-' || isdigit((unsigned char)c)) return parse_json_number(p);
    json_fatal("unexpected character");
    return asnix_make_imm(ASNIX_IMM_NULL); /* unreachable -- json_fatal exits */
}

static Value asnix_json_parse(const char *src, size_t len, int permissive_overflow) {
    JsonParser p = { src, len, 0, permissive_overflow };
    Value out = parse_json_value(&p);
    jp_skip_ws(&p);
    if (p.i != p.len) json_fatal("trailing data after JSON value");
    return out;
}

/* M67: this parser uses one native C stack frame per nesting level (see
 * parse_json_array/parse_json_object/parse_json_value's mutual
 * recursion above) -- fine at ordinary depths, but a sufficiently deep
 * literal (`eval-okay-fromjson-deep.nix` builds `[[[...]]]]` 100,000
 * deep, checking against the real nlohmann-backed `nix-instantiate`'s
 * own comment that it "has no nesting limit") blew the default 8 MiB
 * thread stack and segfaulted -- confirmed directly: the same file's
 * shallower 20,000-deep case already fit and passed, and manually
 * running the whole process under `ulimit -s 65536` (64 MiB) made the
 * 100,000 case pass too, with zero other change.
 *
 * Globally raising the process's own stack (`setrlimit(RLIMIT_STACK,
 * ...)` at startup) was tried first and reverted: it also deepened
 * `builtins.toJSON`'s OWN (unrelated, separate-function) recursive
 * serializer enough to let `eval-fail-toJSON-stack-overflow.nix`
 * actually *finish* instead of crashing -- that conformance case
 * deliberately relies on hitting a stack limit (real Nix has an
 * explicit, interpreter-level max-call-depth counter *independent* of
 * native stack size; this project has no equivalent, so its only
 * "stack overflow" at all is a native one) to prove a controlled
 * failure, so letting it succeed instead flips a passing case to
 * failing. Scoping the bigger stack to ONLY this parser's own
 * recursion -- via a `ucontext.h` side stack, not a new OS thread (no
 * actual concurrency; the caller is suspended for the `swapcontext`
 * duration, same single-threaded execution this whole codebase already
 * assumes elsewhere, e.g. the global arena allocator) -- fixes
 * fromjson-deep without touching toJSON's unrelated call path at all. */
#define ASNIX_JSON_PARSE_STACK_BYTES (64 * 1024 * 1024) /* 64 MiB, see above */

typedef struct {
    const char *src;
    size_t len;
    int permissive_overflow;
    Value result;
} JsonParseCtx;

/* Single global, not re-entrancy-safe for nested concurrent use -- fine
 * here since `swapcontext` below runs this to completion (and any
 * further `fromJSON` calls IT makes, if ever nested, would each get
 * their own fresh JsonParseCtx/stack further down this same call chain,
 * never two live at once from unrelated callers) before the caller
 * resumes. */
static JsonParseCtx *g_json_parse_ctx;

static void json_parse_trampoline(void) {
    g_json_parse_ctx->result =
        asnix_json_parse(g_json_parse_ctx->src, g_json_parse_ctx->len, g_json_parse_ctx->permissive_overflow);
}

static Value run_json_parse_on_big_stack(const char *src, size_t len, int permissive_overflow) {
    JsonParseCtx ctx = { src, len, permissive_overflow, asnix_make_imm(ASNIX_IMM_NULL) };
    JsonParseCtx *saved_ctx = g_json_parse_ctx; /* restore on return, supporting nested fromJSON calls */
    g_json_parse_ctx = &ctx;

    void *stack_mem = malloc(ASNIX_JSON_PARSE_STACK_BYTES);
    ucontext_t caller_ctx, parse_ctx;
    getcontext(&parse_ctx);
    parse_ctx.uc_stack.ss_sp = stack_mem;
    parse_ctx.uc_stack.ss_size = ASNIX_JSON_PARSE_STACK_BYTES;
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
    asnix_bshared_string_bytes(args[0], &bytes, &len, "fromJSON");
    return run_json_parse_on_big_stack(bytes, (size_t)len, /* permissive_overflow */ 0);
}

static Value build_fromJSON(void) { return asnix_bshared_make_primop((void *)prim_fromJSON, 1); }

const AsnixBuiltinMiscDef ASNIX_JSON_BUILTINS[] = {
    { "fromJSON", build_fromJSON },
};

/* M80: entry point for derivation.c's own `__json` attribute handling --
 * see `permissive_overflow`'s header comment (this same file, top of
 * `JsonParser`) for why this needs the big-stack machinery AND a parse
 * policy distinct from `builtins.fromJSON`'s `prim_fromJSON` above,
 * rather than being able to just call that primop's own Value-wrapping
 * entry point. Fatal (exit 1) on any malformed JSON, same as fromJSON --
 * exactly what eval-fail-json-attr-not-json.nix needs. */
Value asnix_json_parse_structured_attrs(const char *bytes, int64_t len) {
    return run_json_parse_on_big_stack(bytes, (size_t)len, /* permissive_overflow */ 1);
}

const size_t ASNIX_JSON_BUILTINS_COUNT = sizeof(ASNIX_JSON_BUILTINS) / sizeof(ASNIX_JSON_BUILTINS[0]);
