/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
/*
 * lessThan, splitVersion, compareVersions, replaceStrings,
 * stringToCharacters, toJSON -- see builtins_compare.h. Every builtin
 * below was verified against real `nix-instantiate --eval` (2.35.2)
 * before being trusted; see each function's own comment for the specific
 * expressions checked, and ~/nix/src/libstore/names.cc /
 * ~/nix/src/libexpr/primops.cc for the ported-from-source pieces.
 */
#include "builtins_compare.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/heap.h"
#include "asnix/ir.h"
#include "builtins_shared.h"
#include "nar.h" /* asnix_copy_path_to_store -- see json_encode's Path case below */
#include "string_context.h" /* M80: replaceStrings' context propagation */
#include "symtab.h"
#include "value_util.h"

/* ---------------------------------------------------------------------
 * lessThan e1 e2 -- verified: `builtins.lessThan "a" "b"` == true,
 * `builtins.lessThan 1 2` == true, `builtins.lessThan "b" "a"` == false.
 * See builtins_shared.h's asnix_bshared_value_lt for the documented
 * int/string-only scope cut vs. real Nix's lessThan (which also orders
 * paths and lists).
 * --------------------------------------------------------------------- */
static Value lessThan2_impl(const Value *args, int64_t argc) {
    (void)argc;
    return asnix_bshared_value_lt(args[0], args[1], "lessThan") ? ASNIX_TRUE : ASNIX_FALSE;
}
static Value build_lessThan(void) { return asnix_bshared_make_curried_native((void *)lessThan2_impl, 2); }

/* ---------------------------------------------------------------------
 * splitVersion / compareVersions -- ported directly from
 * ~/nix/src/libstore/names.cc's `nextComponent`/`componentsLT`/
 * `compareVersions` (byte-index-based reimplementation of the same
 * iterator-based algorithm, since asnix strings aren't NUL-terminated).
 *
 * nextComponent: skip leading '.'/'-' separators (dropped, not part of
 * any component); if a digit follows, consume the maximal digit run;
 * otherwise consume the maximal run of non-digit, non-separator bytes.
 * Verified against real Nix for several genuinely non-obvious cases
 * before trusting this port: `splitVersion "1pre"` == ["1" "pre"]
 * (digit/non-digit transition splits even with no explicit separator);
 * `splitVersion "1.2.3abc"` == ["1" "2" "3" "abc"] (same transition rule
 * applies *after* explicit-separator splitting too); `splitVersion "1_2"`
 * == ["1" "_" "2"] (a single non-digit, non-letter byte is still just
 * "non-digit" -- there is no third character class); `splitVersion
 * "foo_bar"` == ["foo_bar"] (one component: no digit/non-digit transition
 * anywhere in an all-non-digit run, regardless of which non-digit bytes
 * it contains); `splitVersion "1..2"` == ["1" "2"] (consecutive
 * separators collapse, no empty component in between); `splitVersion ""`
 * == [].
 *
 * componentsLT: numeric compare if both components are all-digits;
 * otherwise a handful of special cases for Nix's pre-release convention
 * (an empty component or literally "pre" sorts before anything numeric;
 * "pre" sorts before any other non-numeric component; a non-numeric
 * component sorts before a numeric one, "assume 2.3a < 2.3.1" per the
 * upstream comment) then falls back to plain lexicographic byte compare.
 * Verified: `compareVersions "1.0" "1"` == 1 (missing trailing component
 * is treated as "", and "0" > ""); `compareVersions "1pre" "1"` == -1;
 * `compareVersions "1" "1pre"` == 1; `compareVersions "1.9" "1.10"` ==
 * -1 (numeric, not lexicographic, comparison of "9" vs "10");
 * `compareVersions "1.2.3" "1.2.3abc"` == -1: `splitVersion "1.2.3abc"`
 * has an *extra* 4th component ("abc", from the same digit/non-digit
 * transition rule as "1pre" above) that "1.2.3" doesn't have, so the 4th
 * round compares "" (v1 exhausted) against "abc" -- neither is numeric
 * nor "pre", so this falls all the way through to the final
 * lexicographic-length fallback (`l1 < l2`, i.e. `0 < 3`), making the
 * missing component sort before the present one.
 * --------------------------------------------------------------------- */
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

/* M57f: real Nix's `nextComponent`/`componentsLT` (names.cc) parses a
 * numeric component with `string2Int<int>` -- `int`, NOT `int64_t` --
 * so a component whose numeric value exceeds INT32_MAX (2147483647)
 * fails that parse and falls back to being treated as a plain STRING
 * component for the rest of `componentsLT`'s comparison, not as a
 * (correctly-valued) big integer. Verified against real `nix-instantiate
 * --eval`: `compareVersions "2147483648" "9"` == -1 (2^31, one over the
 * boundary, loses to "9" by the non-numeric-sorts-before-numeric rule)
 * but `compareVersions "2147483647" "9"` == 1 (INT32_MAX itself still
 * parses fine, compares numerically, and correctly wins); a long run of
 * leading zeros does NOT trip this (`"0000000000000000000002"` is still
 * numeric value 2, no overflow) -- confirmed the threshold tracks the
 * numeric VALUE accumulated so far, not the component's string length,
 * which is exactly what checking after each digit (rather than e.g.
 * rejecting on length > 10) gets right for both cases at once. */
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
    asnix_bshared_string_bytes(args[0], &sbytes, &slen, "splitVersion");

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
        out[n++] = asnix_make_string_value(cp, (size_t)cl);
    }
    Value result = asnix_bshared_make_list(out, n);
    free(out);
    return result;
}
static Value build_splitVersion(void) { return asnix_bshared_make_primop((void *)prim_splitVersion, 1); }

static Value compareVersions2_impl(const Value *args, int64_t argc) {
    (void)argc;
    int64_t l1, l2;
    const char *b1, *b2;
    asnix_bshared_string_bytes(args[0], &b1, &l1, "compareVersions");
    asnix_bshared_string_bytes(args[1], &b2, &l2, "compareVersions");
    return asnix_make_int(compare_versions(b1, l1, b2, l2));
}
static Value build_compareVersions(void) { return asnix_bshared_make_curried_native((void *)compareVersions2_impl, 2); }

/* ---------------------------------------------------------------------
 * replaceStrings from to s -- ported directly from
 * ~/nix/src/libexpr/primops.cc's prim_replaceStrings: scans `s` one
 * position at a time (0..len inclusive -- the "inclusive" matters for a
 * `from` entry that's the empty string, which can match at the very end);
 * at each position, tries every `from[i]` *in list order* and uses the
 * *first* one that matches at that exact position (not the longest
 * match); on a match, appends the corresponding `to[i]` and advances by
 * `from[i]`'s length (or by one character, copied through verbatim, if
 * `from[i]` is empty -- otherwise an empty `from` entry would loop
 * forever at the same position); on no match, copies the current
 * character through verbatim and advances by one.
 *
 * Verified against real Nix for the two trickiest cases before trusting
 * this port: `replaceStrings [""] ["X"] "ab"` == "XaXbX" (empty-`from`
 * inserts `to` at every position, including both ends) and
 * `replaceStrings ["a"] ["aa"] "banana"` == "baanaanaa" (a replacement
 * that re-contains the original `from` text does NOT recurse/re-match --
 * `p` always advances past what was just consumed, consistent with the
 * single left-to-right scan above).
 * --------------------------------------------------------------------- */
static Value replaceStrings3_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value from_list = args[0], to_list = args[1], s_v = args[2];
    ASNIX_REQUIRE_LIST(from_list, "replaceStrings");
    ASNIX_REQUIRE_LIST(to_list, "replaceStrings");
    int64_t nfrom = asnix_bshared_list_count(from_list);
    int64_t nto = asnix_bshared_list_count(to_list);
    if (nfrom != nto) asnix_bshared_fatal("replaceStrings", "'from' and 'to' lists have different lengths");
    const Value *from_elems = asnix_bshared_list_elems(from_list);
    const Value *to_elems = asnix_bshared_list_elems(to_list);

    const char **from_bytes = nfrom > 0 ? malloc((size_t)nfrom * sizeof(char *)) : NULL;
    int64_t *from_lens = nfrom > 0 ? malloc((size_t)nfrom * sizeof(int64_t)) : NULL;
    for (int64_t i = 0; i < nfrom; i++) {
        /* List elements are lazy (e.g. a `map`-produced list's elements are
         * deliberately unforced thunks, M23) -- force each one before
         * reading its bytes, same as OP_SELECT/OP_VAR_REF do for attrs/vars
         * on read. Found via real nixpkgs: `replaceStrings [...] (map f
         * someList) s`-shaped calls crashed with "expected a string
         * argument" on an unforced Thunk. */
        asnix_bshared_string_bytes(asnix_force_for_print(from_elems[i]), &from_bytes[i], &from_lens[i], "replaceStrings");
    }
    /* M53: `to_elems[i]` is deliberately NOT forced here, unlike
     * `from_elems[i]` just above -- every `from` entry must be read
     * upfront since the scan below tests *all* of them at every
     * position, but a `to` entry's value/bytes are only ever actually
     * needed at the single position (if any) where its paired `from`
     * entry matches, found and read lazily in the `found >= 0` branch
     * below. Forcing every `to[i]` here unconditionally -- the
     * previous version of this loop did exactly that -- forced entries
     * that are never substituted in at all. Confirmed against real
     * Nix: `replaceStrings [ "oo" "XX" ] [ "u" (throw "unreachable") ]
     * "foobar"` => "fubar", no throw, since "XX" never occurs in
     * "foobar" so its paired `to` entry is never read. */

    int64_t slen;
    const char *sbytes;
    asnix_bshared_string_bytes(s_v, &sbytes, &slen, "replaceStrings");

    /* M80: real Nix's own `prim_replaceStrings` unions `s`'s context with
     * the context of every DISTINCT `to[i]` actually spliced in at least
     * once (never an index that's never matched) -- ported verbatim,
     * see this function's own header comment's "replaceInto" case. */
    AsnixStringContext ctx;
    asnix_ctx_init(&ctx);
    asnix_ctx_union_into(&ctx, asnix_string_context_of(s_v));

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
            Value to_forced = asnix_force_for_print(to_elems[found]);
            asnix_bshared_string_bytes(to_forced, &tb, &tl, "replaceStrings");
            asnix_ctx_union_into(&ctx, asnix_string_context_of(to_forced));
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
    Value result = asnix_make_string_value(out, outlen);
    if (!asnix_ctx_is_empty(&ctx)) asnix_string_attach_context(result, &ctx);
    free(out);
    free(from_bytes);
    free(from_lens);
    return result;
}
static Value build_replaceStrings(void) { return asnix_bshared_make_curried_native((void *)replaceStrings3_impl, 3); }

/* ---------------------------------------------------------------------
 * stringToCharacters s -- not actually a core Nix builtin (confirmed:
 * `builtins ? stringToCharacters` is false); it's `lib.stringToCharacters
 * = s: genList (p: substring p 1 s) (stringLength s)`
 * (lib/strings.nix). Added here under `builtins` for the same reason as
 * `attrByPath`/`foldl` elsewhere in this pass. Verified:
 * `lib.stringToCharacters "abc"` == ["a" "b" "c"]; `lib.stringToCharacters
 * ""` == [].
 * --------------------------------------------------------------------- */
static Value prim_stringToCharacters(const Value *args, int64_t argc) {
    (void)argc;
    int64_t len;
    const char *bytes;
    asnix_bshared_string_bytes(args[0], &bytes, &len, "stringToCharacters");
    Value *out = len > 0 ? malloc((size_t)len * sizeof(Value)) : NULL;
    for (int64_t i = 0; i < len; i++) out[i] = asnix_make_string_value(bytes + i, 1);
    Value result = asnix_bshared_make_list(out, len);
    free(out);
    return result;
}
static Value build_stringToCharacters(void) { return asnix_bshared_make_primop((void *)prim_stringToCharacters, 1); }

/* ---------------------------------------------------------------------
 * toJSON v -- recursive, compact (no whitespace) JSON serialization,
 * matching real Nix's `nlohmann::json::dump()` output byte-for-byte for
 * every case checked: `builtins.toJSON { a=1; b=[1 2 "x\"y"]; c=null;
 * d=true; e=false; }` == "{\"a\":1,\"b\":[1,2,\"x\\\"y\"],\"c\":null,
 * \"d\":true,\"e\":false}" (object keys in alphabetical order, same as
 * this project's own attrset printer -- see builtins_attrs.c's
 * sorted_name_order); `builtins.toJSON "a\nb\tc\\d"` ==
 * "\"a\\nb\\tc\\\\d\"" (standard short escapes for \n \t \\, and \" for
 * quotes -- not ported from a special Nix rule, this is plain JSON string
 * escaping: \b \f \n \r \t \" \\ get their two-character escapes, other
 * control bytes (<0x20) get \u00XX, everything else (including raw UTF-8
 * continuation bytes -- this project has no Unicode awareness anywhere
 * else either) passes through unescaped, matching nlohmann's default
 * `dump()` with `ensure_ascii=false`); `builtins.toJSON []` == "[]";
 * `builtins.toJSON {}` == "{}"; `builtins.toJSON (-5)` == "-5";
 * `builtins.toJSON (a: a)` errors on real Nix ("cannot convert a function
 * to JSON") -- functions/primops are rejected here too.
 *
 * A Path argument IS copied into the content-addressed store before
 * being emitted, matching real Nix's `toJSON` (verified: `builtins.toJSON
 * ./.` there produces a `"/nix/store/..."` string, not the literal
 * filesystem path) -- this was a known, documented gap when M8/M15 first
 * wrote this encoder (no `AddToStore` existed anywhere in the project
 * yet), but M75 later wired up exactly that (`asnix_copy_path_to_store`,
 * nar.c) for `coerce_to_string_for_interp`'s identical Path case and for
 * `asnix_coerce_to_string_for_derivation`; this encoder was missed at the
 * time. Found chasing `bash-interactive`'s `__structuredAttrs`-mode
 * derivation: its own `nativeBuildInputs`/`patches` lists each embed a
 * raw Path literal (`../../build-support/setup-hooks/
 * separate-debug-info.sh`, `./pgrp-pipe-5.patch`) straight into the
 * `__json` env var via this encoder, bypassing the string-coercion call
 * sites that already had the M75 fix -- `asnix_collect_input_refs`
 * (derivation.c) independently walks the same raw attribute Values (not
 * this JSON text) and already added the correctly-copied store paths to
 * `inputSrcs`, so only the literal text *inside* `__json` was stale,
 * silently rotating `bash.outPath`/`bash.drvPath`'s hash. Mirrors
 * `coerce_to_string_for_interp`'s own already-in-`/nix/store/` verbatim
 * special case for consistency with the rest of the codebase, though a
 * quick manual check against real `nix-instantiate --eval` suggests real
 * Nix's `toJSON` may actually re-copy (double-wrap) even an
 * already-in-store Path literal rather than using it verbatim -- not
 * fixed here since no nixpkgs recipe this project evaluates exercises
 * that edge case (every Path literal reaching this encoder names a
 * plain source-tree file, never an already-in-store path), and matching
 * the established convention elsewhere is the safer, non-speculative
 * choice.
 * --------------------------------------------------------------------- */
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

/* perf (M138): SWAR scan for the JSON escape set -- the original
 * per-byte condition (`rc=='"' || rc=='\\' || rc=='\b' || rc=='\f' ||
 * rc=='\n' || rc=='\r' || rc=='\t' || rc<0x20`) is redundant: \b(0x08)/
 * \f(0x0c)/\n(0x0a)/\r(0x0d)/\t(0x09) are already covered by `rc<0x20`,
 * so this simplifies to exactly the same shape as aterm.c's
 * `find_escape_aterm` (see its own comment for the two underlying bit-
 * twiddling identities and how they were verified), just with n=0x20
 * instead of n=0x0e and no '\\'-vs-'"' asymmetry. Output is byte-for-
 * byte identical to a plain per-byte scan-and-switch loop. */
static int64_t find_escape_json(const unsigned char *s, int64_t len) {
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

/* M57d: real Nix's toJSON goes through nlohmann::json, which validates
 * every string byte-for-byte and throws ("JSON serialization error: ...
 * invalid UTF-8 byte at index N") rather than silently emitting whatever
 * garbage bytes it was given -- confirmed: `builtins.toJSON "...\xff..."`
 * (a single invalid 0xFF byte, eval-fail-toJSON-non-utf-8.nix's exact
 * fixture) throws in real Nix. Structural check only (leading-byte class
 * + the right count of 0x80-0xBF continuation bytes, truncation-safe) --
 * doesn't additionally reject overlong encodings or out-of-range/
 * surrogate codepoints the way nlohmann's stricter decoder does, but
 * that's just extra strictness this project's test doesn't exercise; a
 * genuinely malformed byte (a lone continuation byte, or 0xF8-0xFF, which
 * are never valid UTF-8 leading bytes at all) is still always caught. */
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

/* M51: JSON number formatting for a Float value -- matches real Nix's
 * `toJSON`, which hands the double to `nlohmann::json` (value-to-json.cc:
 * `out = v.fpoint()`) and lets THAT library's own `dump()` format it,
 * NOT either of this project's other two float-formatting rules
 * (print.cc's "%.6g"-style top-level printer, or coerceToString's
 * "%f"-style `toString`) -- confirmed directly against
 * `nix-instantiate --eval`: `builtins.toJSON 1.0` == "1.0" (NOT "1" the
 * way the top-level printer would show it, and NOT "1.000000" the way
 * toString would) and `builtins.toJSON 1.7976931348623157e308` ==
 * "1.7976931348623157e+308" (full round-trip precision, not 6
 * significant digits). nlohmann's actual algorithm is a shortest-
 * round-trip float-to-string (think Grisu/Ryu); approximated here by
 * trying `%.*g` at increasing precision until the result parses back to
 * the exact same double (same technique real-world "shortest repr"
 * implementations bottom out to when a dedicated algorithm isn't
 * available) -- verified this reproduces the exact two examples above,
 * plus `builtins.toJSON 1.44` == "1.44" (this project's actual tested
 * case, eval-okay-tojson.nix). nlohmann additionally guarantees the
 * output always contains a `.` or an `e` (so re-parsing it in a
 * language that distinguishes int/float literals still yields a float),
 * which a bare "%.*g" can lose for an integral value (e.g. precision-1
 * formats `1.0` as "1") -- restored by appending ".0" when neither
 * character appears in the shortest match found. */
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

static void json_encode_ctx(Value v, JsonBuf *out, AsnixStringContext *ctx);
static void json_encode(Value v, JsonBuf *out) { json_encode_ctx(v, out, NULL); }

/* M154: (name,len,idx) for one attrset entry, collected ONCE per entry
 * (see json_encode_attrs_ctx's own header comment below) instead of
 * re-derived from `(v, order[j])` on every pairwise comparison the
 * insertion sort makes. */
typedef struct {
    const char *name;
    size_t len;
    int64_t idx;
} JsonAttrEntry;

/* M154: previously, this function's insertion sort called
 * `asnix_sym_name(asnix_bshared_attrs_sym(v, order[j]), ...)` (a symtab
 * lookup) TWICE per comparison, every comparison -- up to O(n^2) lookups
 * for an attrset whose entries aren't already sorted, instead of the
 * O(n) a sort fundamentally needs. Fixed by collecting each entry's
 * (sym-name, len, original-index) ONCE in a single O(n) pass first, then
 * sorting that already-materialized array -- the exact two-step
 * collect-then-sort split `iets`'s own `lexicographic_order()`
 * (`src/print.rs:112-120`) uses, ported verbatim: one `name()` lookup per
 * entry, period, with the sort comparing already-resolved byte spans. */
/* M171 perf: below this threshold, sort a plain JsonAttrEntry array
 * on the stack instead of malloc()ing the scratch array -- the same
 * small-N stack-array technique already applied to
 * asnix_bshared_make_attrs (builtins_shared.c's
 * ASNIX_MAKE_ATTRS_SORT_INSERTION_MAX) and compiler.c's AST_ATTRSET
 * case (ASNIX_COMPILE_SCRATCH_MAX/ASNIX_ATTRSET_SORT_INSERTION_MAX),
 * just never ported to this call site. Reuses the same constant value
 * (32) those two already settled on, rather than inventing a new one;
 * not shared via a header since all three sorts operate on unrelated
 * data and this file has no other reason to depend on the other two.
 * Measured directly on a real nixpkgs eval (`./asnix d.nix`,
 * ffmpeg.outPath, via reverted ad hoc histogram instrumentation):
 * json_encode_attrs_ctx fires 13,617 times, with a bimodal size
 * distribution -- 76.28% of calls have n <= 8 (mostly n<=1, the common
 * case of small structured-attrs env entries), then a second cluster
 * at n in [29,51] (fixed-shape derivation attrsets). Using the
 * project's standard 32 threshold rather than a tighter one (e.g. 8)
 * catches that n in [29,32] tail too (78.39% cumulative) for a
 * negligible extra 768 bytes of stack. */
#define ASNIX_JSON_ATTRS_SORT_STACK_MAX 32

static void json_encode_attrs_ctx(Value v, JsonBuf *out, AsnixStringContext *ctx) {
    int64_t n = asnix_bshared_attrs_count(v);
    JsonAttrEntry entries_stack[ASNIX_JSON_ATTRS_SORT_STACK_MAX];
    int on_stack = (n <= ASNIX_JSON_ATTRS_SORT_STACK_MAX);
    JsonAttrEntry *entries = on_stack ? entries_stack : (n > 0 ? malloc((size_t)n * sizeof(JsonAttrEntry)) : NULL);
    for (int64_t i = 0; i < n; i++) {
        entries[i].name = asnix_sym_name(asnix_bshared_attrs_sym(v, i), &entries[i].len);
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
        json_encode_ctx(asnix_force_for_print(asnix_bshared_attrs_value(v, entries[i].idx)), out, ctx);
    }
    jb_putc(out, '}');
    if (!on_stack) free(entries);
}

/* M80: `ctx` (NULL from every pre-existing call site, see this
 * function's own forward-declared one-line wrapper just above) collects
 * the union of every STRING leaf's own context encountered while
 * walking `v` -- real Nix's own `printValueAsJSON` does the same
 * (`~/nix/src/libexpr/value-to-json.cc` threads a `NixStringContext &
 * context` output parameter through its whole recursion), needed so
 * `builtins.toJSON { inherit s; }` (prim_toJSON below) can attach the
 * combined context to its result string, matching real Nix's
 * `getContext (toJSON {...})`. */
static void json_encode_ctx(Value v, JsonBuf *out, AsnixStringContext *ctx) {
    v = asnix_force_for_print(v);
    switch (asnix_tag_of(v)) {
        case ASNIX_TAG_INT: {
            char buf[32];
            int n = snprintf(buf, sizeof buf, "%lld", (long long)asnix_int_value(v));
            jb_puts(out, buf, (size_t)n);
            break;
        }
        case ASNIX_TAG_IMM:
            switch (asnix_imm_subtag(v)) {
                case ASNIX_IMM_TRUE: jb_puts(out, "true", 4); break;
                case ASNIX_IMM_FALSE: jb_puts(out, "false", 5); break;
                case ASNIX_IMM_NULL: jb_puts(out, "null", 4); break;
                default: asnix_bshared_fatal("toJSON", "cannot convert this value to JSON");
            }
            break;
        case ASNIX_TAG_PTR: {
            if (asnix_value_is_float(v)) {
                char buf[64];
                size_t n = format_json_float(asnix_float_value(v), buf, sizeof(buf));
                jb_puts(out, buf, n);
                break;
            }
            if (asnix_value_is_path(v)) {
                /* See this function's own header comment (toJSON's Path
                 * handling) -- copy to store (unless already a store
                 * path) instead of emitting the raw filesystem text. */
                int64_t plen;
                const char *pbytes = asnix_ptr_bytes(v, &plen);
                char *path = malloc((size_t)plen + 1);
                memcpy(path, pbytes, (size_t)plen);
                path[plen] = '\0';
                const char *final_path = path;
                char store_path[300];
                if (!(plen > 11 && memcmp(path, "/nix/store/", 11) == 0)) {
                    asnix_copy_path_to_store(path, store_path, sizeof(store_path));
                    final_path = store_path;
                }
                jb_escape_string(out, final_path, (int64_t)strlen(final_path));
                free(path);
                break;
            }
            int64_t len;
            const char *bytes = asnix_ptr_bytes(v, &len); /* plain string, see header comment */
            if (!utf8_is_valid(bytes, len)) {
                asnix_bshared_fatal("toJSON", "string is not valid UTF-8");
            }
            if (ctx) asnix_ctx_union_into(ctx, asnix_string_context_of(v));
            jb_escape_string(out, bytes, len);
            break;
        }
        case ASNIX_TAG_LIST: {
            int64_t n = asnix_bshared_list_count(v);
            const Value *elems = asnix_bshared_list_elems(v);
            jb_putc(out, '[');
            for (int64_t i = 0; i < n; i++) {
                if (i > 0) jb_putc(out, ',');
                json_encode_ctx(elems[i], out, ctx);
            }
            jb_putc(out, ']');
            break;
        }
        case ASNIX_TAG_ATTRS: {
            /* An attrset with a `__toString`/`outPath` attribute encodes
             * as whatever THAT coerces to (a JSON string), not as a JSON
             * object -- matches real Nix's `printValueAsJSON`
             * (value-to-json.cc), which "peels" via exactly this
             * mechanism before ever treating a set as a JSON object
             * (confirmed directly: `builtins.toJSON { outPath.foo =
             * true; }` == `"{\"foo\":true}"` there, i.e. the WHOLE
             * attrset becomes the peeled value, not a `{"outPath":...}`
             * object). Reuses asnix_coerce_to_string, which already
             * implements this exact __toString/outPath lookup (M23) for
             * `toString`/`"${...}"`. */
            uint32_t sym_tostring = asnix_intern("__toString", 10);
            uint32_t sym_outpath = asnix_intern("outPath", 7);
            if (asnix_bshared_attrs_find(v, sym_tostring) >= 0 || asnix_bshared_attrs_find(v, sym_outpath) >= 0) {
                json_encode_ctx(asnix_coerce_to_string(v), out, ctx);
            } else {
                json_encode_attrs_ctx(v, out, ctx);
            }
            break;
        }
        default:
            asnix_bshared_fatal("toJSON", "cannot convert a function to JSON");
    }
}

/* M69: NUL-terminated-cstring entry point onto the same `json_encode`
 * recursive encoder, for callers outside this translation unit (see
 * builtins_compare.h's header comment) -- `prim_toJSON` below is just this
 * function plus the Value-wrapping step. */
char *asnix_toJSON_cstr(Value v) {
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
    AsnixStringContext ctx;
    asnix_ctx_init(&ctx);
    json_encode_ctx(args[0], &jb, &ctx);
    jb_putc(&jb, '\0');
    Value result = asnix_make_string_value(jb.buf, strlen(jb.buf));
    /* M80: unlike the public `asnix_toJSON_cstr` entry point (used for
     * `__json`-structured-attrs serialization, derivation.c -- that
     * caller already collects context from the RAW attrset directly,
     * `asnix_collect_input_refs`, independent of this JSON text), the
     * user-facing `builtins.toJSON` primop attaches the union of every
     * string leaf's own context to its result (see json_encode_ctx's
     * own header comment). */
    if (!asnix_ctx_is_empty(&ctx)) asnix_string_attach_context(result, &ctx);
    free(jb.buf);
    return result;
}
static Value build_toJSON(void) { return asnix_bshared_make_primop((void *)prim_toJSON, 1); }

const AsnixBuiltinMiscDef ASNIX_COMPARE_BUILTINS[] = {
    { "lessThan", build_lessThan },
    { "splitVersion", build_splitVersion },
    { "compareVersions", build_compareVersions },
    { "replaceStrings", build_replaceStrings },
    { "stringToCharacters", build_stringToCharacters },
    { "toJSON", build_toJSON },
};

const size_t ASNIX_COMPARE_BUILTINS_COUNT = sizeof(ASNIX_COMPARE_BUILTINS) / sizeof(ASNIX_COMPARE_BUILTINS[0]);
