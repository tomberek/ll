/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "builtins_compare.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ll/heap.h"
#include "ll/ir.h"
#include "builtins_shared.h"
#include "nar.h" /* ll_copy_path_to_store -- see json_encode's Path case below */
#include "string_context.h" /* replaceStrings' context propagation */
#include "symtab.h"
#include "value_util.h"

static Value lessThan2_impl(const Value *args, int64_t argc) {
    (void)argc;
    return ll_bshared_value_lt(args[0], args[1], "lessThan") ? LL_TRUE : LL_FALSE;
}
static Value build_lessThan(void) { return ll_bshared_make_curried_native((void *)lessThan2_impl, 2); }

/* splitVersion / compareVersions -- ported directly from
 * ~/nix/src/libstore/names.cc's `nextComponent`/`componentsLT`/
 * `compareVersions` (byte-index-based reimplementation of the same
 * iterator-based algorithm, since asnix strings aren't NUL-terminated).
 *
 * nextComponent: skip leading '.'/'-' separators (dropped, not part of
 * any component); if a digit follows, consume the maximal digit run;
 * otherwise consume the maximal run of non-digit, non-separator bytes.
 *
 * componentsLT: numeric compare if both components are all-digits;
 * otherwise a handful of special cases for Nix's pre-release convention
 * (an empty component or literally "pre" sorts before anything numeric;
 * "pre" sorts before any other non-numeric component; a non-numeric
 * component sorts before a numeric one) then falls back to plain
 * lexicographic byte compare. */
static void next_component(const char *s, int64_t len, int64_t *pos, const char **out_ptr, int64_t *out_len) {
    int64_t p = *pos;
    while (p < len && (s[p] == '.' || s[p] == '-')) p++;
    if (p >= len) {
        *out_ptr = s + p;
        *out_len = 0;
        *pos = p;
        return;
    }
    int64_t start = p;
    if (s[p] >= '0' && s[p] <= '9') {
        while (p < len && s[p] >= '0' && s[p] <= '9') p++;
    } else {
        while (p < len && !(s[p] >= '0' && s[p] <= '9') && s[p] != '.' && s[p] != '-') p++;
    }
    *out_ptr = s + start;
    *out_len = p - start;
    *pos = p;
}

/* Real Nix's `nextComponent`/`componentsLT` (names.cc) parses a numeric
 * component with `string2Int<int>` -- `int`, NOT `int64_t` -- so a
 * component whose numeric value exceeds INT32_MAX (2147483647) fails
 * that parse and falls back to being treated as a plain STRING
 * component for the rest of `componentsLT`'s comparison, not as a
 * (correctly-valued) big integer. Do not "fix" this to handle large
 * integers correctly -- that would diverge from real Nix's behavior. */
static int component_is_int(const char *s, int64_t len, int64_t *out) {
    if (len == 0) return 0;
    int64_t v = 0;
    for (int64_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10 + (s[i] - '0');
        if (v > 2147483647LL) return 0;
    }
    *out = v;
    return 1;
}

static int components_lt(const char *c1, int64_t l1, const char *c2, int64_t l2) {
    int64_t n1, n2;
    int is1 = component_is_int(c1, l1, &n1);
    int is2 = component_is_int(c2, l2, &n2);
    if (is1 && is2) return n1 < n2;
    if (l1 == 0 && is2) return 1;
    int c1_is_pre = (l1 == 3 && memcmp(c1, "pre", 3) == 0);
    int c2_is_pre = (l2 == 3 && memcmp(c2, "pre", 3) == 0);
    if (c1_is_pre && !c2_is_pre) return 1;
    if (c2_is_pre) return 0;
    if (is2) return 1; /* "2.3a" < "2.3.1" */
    if (is1) return 0;
    int64_t n = l1 < l2 ? l1 : l2;
    int c = n > 0 ? memcmp(c1, c2, (size_t)n) : 0;
    if (c != 0) return c < 0;
    return l1 < l2;
}

static int compare_versions(const char *v1, int64_t len1, const char *v2, int64_t len2) {
    int64_t p1 = 0, p2 = 0;
    while (p1 < len1 || p2 < len2) {
        const char *c1p, *c2p;
        int64_t c1l, c2l;
        next_component(v1, len1, &p1, &c1p, &c1l);
        next_component(v2, len2, &p2, &c2p, &c2l);
        if (components_lt(c1p, c1l, c2p, c2l)) return -1;
        if (components_lt(c2p, c2l, c1p, c1l)) return 1;
    }
    return 0;
}

static Value prim_splitVersion(const Value *args, int64_t argc) {
    (void)argc;
    int64_t slen;
    const char *sbytes;
    ll_bshared_string_bytes(args[0], &sbytes, &slen, "splitVersion");

    Value *out = NULL;
    int64_t cap = 0, n = 0, pos = 0;
    while (pos < slen) {
        const char *cp;
        int64_t cl;
        next_component(sbytes, slen, &pos, &cp, &cl);
        if (cl == 0) break;
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            out = realloc(out, (size_t)cap * sizeof(Value));
        }
        out[n++] = ll_make_string_value(cp, (size_t)cl);
    }
    Value result = ll_bshared_make_list(out, n);
    free(out);
    return result;
}
static Value build_splitVersion(void) { return ll_bshared_make_primop((void *)prim_splitVersion, 1); }

static Value compareVersions2_impl(const Value *args, int64_t argc) {
    (void)argc;
    int64_t l1, l2;
    const char *b1, *b2;
    ll_bshared_string_bytes(args[0], &b1, &l1, "compareVersions");
    ll_bshared_string_bytes(args[1], &b2, &l2, "compareVersions");
    return ll_make_int(compare_versions(b1, l1, b2, l2));
}
static Value build_compareVersions(void) { return ll_bshared_make_curried_native((void *)compareVersions2_impl, 2); }

/* replaceStrings from to s -- ported directly from
 * ~/nix/src/libexpr/primops.cc's prim_replaceStrings: scans `s` one
 * position at a time (0..len inclusive -- the "inclusive" matters for a
 * `from` entry that's the empty string, which can match at the very end);
 * at each position, tries every `from[i]` *in list order* and uses the
 * *first* one that matches at that exact position (not the longest
 * match); on a match, appends the corresponding `to[i]` and advances by
 * `from[i]`'s length (or by one character, copied through verbatim, if
 * `from[i]` is empty -- otherwise an empty `from` entry would loop
 * forever at the same position); on no match, copies the current
 * character through verbatim and advances by one. */
static Value replaceStrings3_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value from_list = args[0], to_list = args[1], s_v = args[2];
    LL_REQUIRE_LIST(from_list, "replaceStrings");
    LL_REQUIRE_LIST(to_list, "replaceStrings");
    int64_t nfrom = ll_bshared_list_count(from_list);
    int64_t nto = ll_bshared_list_count(to_list);
    if (nfrom != nto) ll_bshared_fatal("replaceStrings", "'from' and 'to' lists have different lengths");
    const Value *from_elems = ll_bshared_list_elems(from_list);
    const Value *to_elems = ll_bshared_list_elems(to_list);

    const char **from_bytes = nfrom > 0 ? malloc((size_t)nfrom * sizeof(char *)) : NULL;
    int64_t *from_lens = nfrom > 0 ? malloc((size_t)nfrom * sizeof(int64_t)) : NULL;
    for (int64_t i = 0; i < nfrom; i++) {
        /* List elements are lazy (e.g. a `map`-produced list's elements are
         * deliberately unforced thunks) -- force each one before reading
         * its bytes, same as OP_SELECT/OP_VAR_REF do for attrs/vars on
         * read. */
        ll_bshared_string_bytes(ll_force_for_print(from_elems[i]), &from_bytes[i], &from_lens[i], "replaceStrings");
    }
    /* `to_elems[i]` is deliberately NOT forced here, unlike `from_elems[i]`
     * just above -- every `from` entry must be read upfront since the
     * scan below tests *all* of them at every position, but a `to`
     * entry's value/bytes are only ever actually needed at the single
     * position (if any) where its paired `from` entry matches, found and
     * read lazily in the `found >= 0` branch below. Forcing every `to[i]`
     * unconditionally would force entries that are never substituted in
     * at all -- `to[i]` may legitimately be something like
     * `throw "unreachable"` that must never be evaluated if its `from[i]`
     * never matches. */

    int64_t slen;
    const char *sbytes;
    ll_bshared_string_bytes(s_v, &sbytes, &slen, "replaceStrings");

    /* Unions `s`'s context with the context of every DISTINCT `to[i]`
     * actually spliced in at least once (never an index that's never
     * matched) -- matches real Nix's own `prim_replaceStrings`. */
    LlStringContext ctx;
    ll_ctx_init(&ctx);
    ll_ctx_union_into(&ctx, ll_string_context_of(s_v));

    size_t cap = (size_t)slen + 16, outlen = 0;
    char *out = malloc(cap);
    int64_t p = 0;
    while (p <= slen) {
        int64_t found = -1;
        for (int64_t i = 0; i < nfrom; i++) {
            int64_t fl = from_lens[i];
            if (p + fl <= slen && (fl == 0 || memcmp(sbytes + p, from_bytes[i], (size_t)fl) == 0)) {
                found = i;
                break;
            }
        }
        if (found >= 0) {
            const char *tb;
            int64_t tl;
            Value to_forced = ll_force_for_print(to_elems[found]);
            ll_bshared_string_bytes(to_forced, &tb, &tl, "replaceStrings");
            ll_ctx_union_into(&ctx, ll_string_context_of(to_forced));
            if (outlen + (size_t)tl + 1 > cap) {
                cap = (outlen + (size_t)tl + 1) * 2;
                out = realloc(out, cap);
            }
            if (tl > 0) memcpy(out + outlen, tb, (size_t)tl);
            outlen += (size_t)tl;
            if (from_lens[found] == 0) {
                if (p < slen) out[outlen++] = sbytes[p];
                p++;
            } else {
                p += from_lens[found];
            }
        } else {
            if (p < slen) {
                if (outlen + 1 > cap) { cap *= 2; out = realloc(out, cap); }
                out[outlen++] = sbytes[p];
            }
            p++;
        }
    }
    Value result = ll_make_string_value(out, outlen);
    if (!ll_ctx_is_empty(&ctx)) ll_string_attach_context(result, &ctx);
    free(out);
    free(from_bytes);
    free(from_lens);
    return result;
}
static Value build_replaceStrings(void) { return ll_bshared_make_curried_native((void *)replaceStrings3_impl, 3); }

static Value prim_stringToCharacters(const Value *args, int64_t argc) {
    (void)argc;
    int64_t len;
    const char *bytes;
    ll_bshared_string_bytes(args[0], &bytes, &len, "stringToCharacters");
    Value *out = len > 0 ? malloc((size_t)len * sizeof(Value)) : NULL;
    for (int64_t i = 0; i < len; i++) out[i] = ll_make_string_value(bytes + i, 1);
    Value result = ll_bshared_make_list(out, len);
    free(out);
    return result;
}
static Value build_stringToCharacters(void) { return ll_bshared_make_primop((void *)prim_stringToCharacters, 1); }

/* toJSON v -- recursive, compact (no whitespace) JSON serialization,
 * matching real Nix's `nlohmann::json::dump()` output: object keys in
 * alphabetical order (see builtins_attrs.c's sorted_name_order);
 * standard JSON string escaping (\b \f \n \r \t \" \\, other control
 * bytes as \u00XX); functions/primops are rejected.
 *
 * A Path argument IS copied into the content-addressed store before
 * being emitted, matching real Nix's `toJSON` (verified: `builtins.toJSON
 * ./.` produces a `/nix/store/...` string, not the literal filesystem
 * path) -- mirrors `coerce_to_string_for_interp`'s own already-in-store
 * verbatim special case. */
typedef struct {
    char *buf;
    size_t len, cap;
} JsonBuf;

static void jb_init(JsonBuf *b) {
    b->cap = 64;
    b->buf = malloc(b->cap);
    b->len = 0;
}

static void jb_ensure(JsonBuf *b, size_t extra) {
    if (b->len + extra <= b->cap) return;
    size_t ncap = b->cap ? b->cap * 2 : 64;
    while (ncap < b->len + extra) ncap *= 2;
    b->buf = realloc(b->buf, ncap);
    b->cap = ncap;
}

static void jb_putc(JsonBuf *b, char c) {
    jb_ensure(b, 1);
    b->buf[b->len++] = c;
}

static void jb_puts(JsonBuf *b, const char *s, size_t n) {
    jb_ensure(b, n);
    memcpy(b->buf + b->len, s, n);
    b->len += n;
}

static int64_t find_escape_json_scalar(const unsigned char *s, int64_t len) {
    static const uint64_t LO = 0x0101010101010101ULL;
    static const uint64_t HI = 0x8080808080808080ULL;
    int64_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t x;
        memcpy(&x, s + i, 8);
        uint64_t q = x ^ (LO * (uint64_t)'"');
        uint64_t bs = x ^ (LO * (uint64_t)'\\');
        uint64_t any = ((x - LO * 0x20) & ~x) | ((q - LO) & ~q) | ((bs - LO) & ~bs);
        if (any & HI) {
            for (int64_t j = i; j < i + 8; j++) {
                unsigned char c = s[j];
                if (c == '"' || c == '\\' || c < 0x20) return j;
            }
        }
    }
    for (; i < len; i++) {
        unsigned char c = s[i];
        if (c == '"' || c == '\\' || c < 0x20) return i;
    }
    return len;
}

#if defined(__x86_64__) || defined(__i386__)
#include <emmintrin.h>

/* SSE2 vector compare, 16 bytes/iteration. "unsigned x < 0x20" has no
 * single SSE2 comparison (only signed `_mm_cmplt_epi8` exists), so this
 * uses an unsigned-safe identity instead: `min_epu8(x, 0x1f) == x` iff
 * `x <= 0x1f`, i.e. `x < 0x20` -- correct for every byte value 0-255,
 * not just ASCII. */
static int64_t find_escape_json(const unsigned char *s, int64_t len) {
    int64_t i = 0;
    for (; i + 16 <= len; i += 16) {
        __m128i x = _mm_loadu_si128((const __m128i *)(s + i));
        __m128i eq_dq = _mm_cmpeq_epi8(x, _mm_set1_epi8('"'));
        __m128i eq_bs = _mm_cmpeq_epi8(x, _mm_set1_epi8('\\'));
        __m128i lt_20 = _mm_cmpeq_epi8(_mm_min_epu8(x, _mm_set1_epi8(0x1f)), x);
        __m128i any = _mm_or_si128(_mm_or_si128(eq_dq, eq_bs), lt_20);
        int mask = _mm_movemask_epi8(any);
        if (mask) return i + __builtin_ctz((unsigned)mask);
    }
    return i + find_escape_json_scalar(s + i, len - i);
}
#else
static int64_t find_escape_json(const unsigned char *s, int64_t len) {
    return find_escape_json_scalar(s, len);
}
#endif

static void jb_escape_string(JsonBuf *out, const char *bytes, int64_t len) {
    jb_putc(out, '"');
    const unsigned char *p = (const unsigned char *)bytes;
    int64_t pos = 0;
    for (;;) {
        int64_t i = pos + find_escape_json(p + pos, len - pos);
        if (i != pos) jb_puts(out, bytes + pos, (size_t)(i - pos));
        if (i == len) break;
        pos = i + 1;
        unsigned char c = p[i];
        switch (c) {
            case '"': jb_puts(out, "\\\"", 2); break;
            case '\\': jb_puts(out, "\\\\", 2); break;
            case '\b': jb_puts(out, "\\b", 2); break;
            case '\f': jb_puts(out, "\\f", 2); break;
            case '\n': jb_puts(out, "\\n", 2); break;
            case '\r': jb_puts(out, "\\r", 2); break;
            case '\t': jb_puts(out, "\\t", 2); break;
            default: {
                char esc[8];
                int n = snprintf(esc, sizeof esc, "\\u%04x", c);
                jb_puts(out, esc, (size_t)n);
                break;
            }
        }
    }
    jb_putc(out, '"');
}

static int utf8_is_valid(const char *bytes, int64_t len) {
    int64_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)bytes[i];
        int extra;
        if (c < 0x80) extra = 0;
        else if ((c & 0xE0) == 0xC0) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0) extra = 3;
        else return 0;
        if (i + extra >= len) return 0;
        for (int k = 1; k <= extra; k++) {
            if (((unsigned char)bytes[i + k] & 0xC0) != 0x80) return 0;
        }
        i += extra + 1;
    }
    return 1;
}

/* JSON number formatting for a Float value -- matches real Nix's
 * `toJSON`, which lets nlohmann::json's own `dump()` format the double
 * (shortest round-trip repr, think Grisu/Ryu), NOT either of this
 * project's other two float-formatting rules (print.cc's top-level
 * printer, or coerceToString's `toString`). Approximated here by trying
 * `%.*g` at increasing precision until the result parses back to the
 * exact same double. nlohmann additionally guarantees the output always
 * contains a `.` or an `e` (so re-parsing it in a language that
 * distinguishes int/float literals still yields a float), which a bare
 * `%.*g` can lose for an integral value (e.g. "1.0" at low precision
 * becomes "1") -- restored by appending ".0" when neither character
 * appears in the shortest match found. */
static size_t format_json_float(double d, char *buf, size_t bufsize) {
    int n = 0;
    for (int prec = 1; prec <= 17; prec++) {
        n = snprintf(buf, bufsize, "%.*g", prec, d);
        if (strtod(buf, NULL) == d) break;
    }
    if (n < 0) return 0;
    size_t len = (size_t)n;
    if (len + 3 > bufsize) return len; /* defensive -- buf is always sized generously by the caller */
    int has_dot_or_exp = 0;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '.' || buf[i] == 'e' || buf[i] == 'E') { has_dot_or_exp = 1; break; }
    }
    if (!has_dot_or_exp) {
        buf[len++] = '.';
        buf[len++] = '0';
        buf[len] = '\0';
    }
    return len;
}

static int json_name_cmp(const char *an, size_t al, const char *bn, size_t bl) {
    size_t n = al < bl ? al : bl;
    int c = n > 0 ? memcmp(an, bn, n) : 0;
    if (c != 0) return c;
    return (al > bl) - (al < bl);
}

static void json_encode_ctx(Value v, JsonBuf *out, LlStringContext *ctx);
static void json_encode(Value v, JsonBuf *out) { json_encode_ctx(v, out, NULL); }

typedef struct {
    const char *name;
    size_t len;
    int64_t idx;
} JsonAttrEntry;

#define LL_JSON_ATTRS_SORT_STACK_MAX 32

static void json_encode_attrs_ctx(Value v, JsonBuf *out, LlStringContext *ctx) {
    int64_t n = ll_bshared_attrs_count(v);
    JsonAttrEntry entries_stack[LL_JSON_ATTRS_SORT_STACK_MAX];
    int on_stack = (n <= LL_JSON_ATTRS_SORT_STACK_MAX);
    JsonAttrEntry *entries = on_stack ? entries_stack : (n > 0 ? malloc((size_t)n * sizeof(JsonAttrEntry)) : NULL);
    for (int64_t i = 0; i < n; i++) {
        entries[i].name = ll_sym_name(ll_bshared_attrs_sym(v, i), &entries[i].len);
        entries[i].idx = i;
    }
    for (int64_t i = 1; i < n; i++) {
        JsonAttrEntry cur = entries[i];
        int64_t j = i;
        while (j > 0 && json_name_cmp(entries[j - 1].name, entries[j - 1].len, cur.name, cur.len) > 0) {
            entries[j] = entries[j - 1];
            j--;
        }
        entries[j] = cur;
    }
    jb_putc(out, '{');
    for (int64_t i = 0; i < n; i++) {
        if (i > 0) jb_putc(out, ',');
        jb_escape_string(out, entries[i].name, (int64_t)entries[i].len);
        jb_putc(out, ':');
        json_encode_ctx(ll_force_for_print(ll_bshared_attrs_value(v, entries[i].idx)), out, ctx);
    }
    jb_putc(out, '}');
    if (!on_stack) free(entries);
}

/* `ctx` (NULL from call sites that don't need it) collects the union of
 * every STRING leaf's own context encountered while walking `v` -- real
 * Nix's own `printValueAsJSON` does the same, needed so
 * `builtins.toJSON` can attach the combined context to its result
 * string, matching real Nix's `getContext (toJSON {...})`. */
static void json_encode_ctx(Value v, JsonBuf *out, LlStringContext *ctx) {
    v = ll_force_for_print(v);
    switch (ll_tag_of(v)) {
        case LL_TAG_INT: {
            char buf[32];
            int n = snprintf(buf, sizeof buf, "%lld", (long long)ll_int_value(v));
            jb_puts(out, buf, (size_t)n);
            break;
        }
        case LL_TAG_IMM:
            switch (ll_imm_subtag(v)) {
                case LL_IMM_TRUE: jb_puts(out, "true", 4); break;
                case LL_IMM_FALSE: jb_puts(out, "false", 5); break;
                case LL_IMM_NULL: jb_puts(out, "null", 4); break;
                default: ll_bshared_fatal("toJSON", "cannot convert this value to JSON");
            }
            break;
        case LL_TAG_PTR: {
            if (ll_value_is_float(v)) {
                char buf[64];
                size_t n = format_json_float(ll_float_value(v), buf, sizeof(buf));
                jb_puts(out, buf, n);
                break;
            }
            if (ll_value_is_path(v)) {
                /* See this function's own header comment (toJSON's Path
                 * handling) -- copy to store (unless already a store
                 * path) instead of emitting the raw filesystem text. */
                int64_t plen;
                const char *pbytes = ll_ptr_bytes(v, &plen);
                char *path = malloc((size_t)plen + 1);
                memcpy(path, pbytes, (size_t)plen);
                path[plen] = '\0';
                const char *final_path = path;
                char store_path[300];
                if (!(plen > 11 && memcmp(path, "/nix/store/", 11) == 0)) {
                    ll_copy_path_to_store(path, store_path, sizeof(store_path));
                    final_path = store_path;
                }
                jb_escape_string(out, final_path, (int64_t)strlen(final_path));
                free(path);
                break;
            }
            int64_t len;
            const char *bytes = ll_ptr_bytes(v, &len); /* plain string, see header comment */
            if (!utf8_is_valid(bytes, len)) {
                ll_bshared_fatal("toJSON", "string is not valid UTF-8");
            }
            if (ctx) ll_ctx_union_into(ctx, ll_string_context_of(v));
            jb_escape_string(out, bytes, len);
            break;
        }
        case LL_TAG_LIST: {
            int64_t n = ll_bshared_list_count(v);
            const Value *elems = ll_bshared_list_elems(v);
            jb_putc(out, '[');
            for (int64_t i = 0; i < n; i++) {
                if (i > 0) jb_putc(out, ',');
                json_encode_ctx(elems[i], out, ctx);
            }
            jb_putc(out, ']');
            break;
        }
        case LL_TAG_ATTRS: {
            /* An attrset with a `__toString`/`outPath` attribute encodes
             * as whatever THAT coerces to (a JSON string), not as a JSON
             * object -- matches real Nix's `printValueAsJSON`, which
             * "peels" via exactly this mechanism before ever treating a
             * set as a JSON object. Reuses ll_coerce_to_string, which
             * already implements this exact __toString/outPath lookup
             * for `toString`/`"${...}"`. */
            uint32_t sym_tostring = ll_intern("__toString", 10);
            uint32_t sym_outpath = ll_intern("outPath", 7);
            if (ll_bshared_attrs_find(v, sym_tostring) >= 0 || ll_bshared_attrs_find(v, sym_outpath) >= 0) {
                json_encode_ctx(ll_coerce_to_string(v), out, ctx);
            } else {
                json_encode_attrs_ctx(v, out, ctx);
            }
            break;
        }
        default:
            ll_bshared_fatal("toJSON", "cannot convert a function to JSON");
    }
}

/* NUL-terminated-cstring entry point onto the same `json_encode`
 * recursive encoder, for callers outside this translation unit --
 * `prim_toJSON` below is just this function plus the Value-wrapping
 * step. */
char *ll_toJSON_cstr(Value v) {
    JsonBuf jb;
    jb_init(&jb);
    json_encode(v, &jb);
    jb_putc(&jb, '\0');
    return jb.buf;
}

static Value prim_toJSON(const Value *args, int64_t argc) {
    (void)argc;
    JsonBuf jb;
    jb_init(&jb);
    LlStringContext ctx;
    ll_ctx_init(&ctx);
    json_encode_ctx(args[0], &jb, &ctx);
    jb_putc(&jb, '\0');
    Value result = ll_make_string_value(jb.buf, strlen(jb.buf));
    /* Unlike the public `ll_toJSON_cstr` entry point (used for
     * `__json`-structured-attrs serialization, which already collects
     * context from the raw attrset directly), the user-facing
     * `builtins.toJSON` primop attaches the union of every string leaf's
     * own context to its result. */
    if (!ll_ctx_is_empty(&ctx)) ll_string_attach_context(result, &ctx);
    free(jb.buf);
    return result;
}
static Value build_toJSON(void) { return ll_bshared_make_primop((void *)prim_toJSON, 1); }

const LlBuiltinMiscDef LL_COMPARE_BUILTINS[] = {
    { "lessThan", build_lessThan },
    { "splitVersion", build_splitVersion },
    { "compareVersions", build_compareVersions },
    { "replaceStrings", build_replaceStrings },
    { "stringToCharacters", build_stringToCharacters },
    { "toJSON", build_toJSON },
};

const size_t LL_COMPARE_BUILTINS_COUNT = sizeof(LL_COMPARE_BUILTINS) / sizeof(LL_COMPARE_BUILTINS[0]);
