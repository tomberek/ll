/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "builtins_flakeref.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ll/arena.h"
#include "ll/ir.h" /* ll_force_for_print */
#include "base64.h"
#include "builtins_shared.h"
#include "symtab.h"
#include "value_util.h"

/* ---------------------------------------------------------------------
 * builtins.parseFlakeRef / builtins.flakeRefToString.
 *
 * This is pure syntax (no filesystem/network/registry access). All
 * intermediate scratch buffers in this file use plain malloc/free (freed
 * before returning) -- final results are always copied into a fresh
 * arena-backed Value via ll_make_string_value/ll_bshared_make_attrs
 * before returning.
 * --------------------------------------------------------------------- */

static void fr_fatal(const char *who, const char *msg) { ll_bshared_fatal(who, msg); }

static _Noreturn void fr_fatalf(const char *who, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ll_bshared_fatal(who, buf);
    exit(1); /* unreachable -- ll_bshared_fatal never returns */
}

/* ---------------------------------------------------------------------
 * Tiny growable byte buffer (malloc/realloc), used to assemble output
 * strings before handing them to ll_make_string_value.
 * --------------------------------------------------------------------- */
typedef struct {
    char *buf;
    int64_t len;
    int64_t cap;
} SB;

static void sb_init(SB *sb) {
    sb->cap = 64;
    sb->buf = malloc((size_t)sb->cap);
    sb->len = 0;
}
static void sb_ensure(SB *sb, int64_t more) {
    if (sb->len + more <= sb->cap) return;
    while (sb->len + more > sb->cap) sb->cap *= 2;
    sb->buf = realloc(sb->buf, (size_t)sb->cap);
}
static void sb_bytes(SB *sb, const char *s, int64_t len) {
    sb_ensure(sb, len);
    memcpy(sb->buf + sb->len, s, (size_t)len);
    sb->len += len;
}
static void sb_cstr(SB *sb, const char *s) { sb_bytes(sb, s, (int64_t)strlen(s)); }
static void sb_char(SB *sb, char c) { sb_bytes(sb, &c, 1); }
static Value sb_finish(SB *sb) {
    Value v = ll_make_string_value(sb->buf, (size_t)sb->len);
    free(sb->buf);
    return v;
}

/* ---------------------------------------------------------------------
 * Percent-decoding / percent-encoding. Three distinct "safe" charsets,
 * reverse-engineered by sweeping every printable ASCII byte through real
 * `nix-instantiate --eval -E 'builtins.flakeRefToString { ... }'` and
 * comparing which bytes survive bare vs get escaped:
 *   CS_SEGMENT: a single opaque path segment (owner/repo/ref/id) --
 *     unreserved (A-Za-z0-9-._~) plus ':' '@' survive bare; EVERYTHING
 *     else (including '/' and '%') is escaped. This is deliberately
 *     narrower than RFC3986 "pchar" -- sub-delims like '!' '$' '&' '('
 *     ')' '*' '+' ',' ';' '=' are escaped here too, confirmed directly
 *     against real Nix, not assumed from the RFC.
 *   CS_QPATH: a generic URL path component (git+/tarball+/file+ url
 *     field) -- same as CS_SEGMENT plus '/' survives bare (real path
 *     separators), '?' does not.
 *   CS_QUERY: a query value -- same as CS_QPATH plus '?' ALSO survives
 *     bare.
 * --------------------------------------------------------------------- */
typedef enum { CS_SEGMENT, CS_QPATH, CS_QUERY } Charset;

static int fr_is_unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
           c == '_' || c == '~';
}
static int fr_is_safe(unsigned char c, Charset cs) {
    if (fr_is_unreserved(c) || c == ':' || c == '@') return 1;
    if (cs == CS_QPATH || cs == CS_QUERY) {
        if (c == '/') return 1;
    }
    if (cs == CS_QUERY) {
        if (c == '?') return 1;
    }
    return 0;
}

static int fr_hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decodes %XX sequences (case-insensitive hex); a stray '%' not followed
 * by two hex digits is passed through literally. Writes into a fresh
 * malloc'd buffer (always no longer than the input), written through
 * `outlen`. */
static char *fr_pct_decode(const char *s, int64_t len, int64_t *outlen) {
    char *out = malloc((size_t)len + 1);
    int64_t o = 0;
    for (int64_t i = 0; i < len; i++) {
        if (s[i] == '%' && i + 2 < len && fr_hex_val(s[i + 1]) >= 0 && fr_hex_val(s[i + 2]) >= 0) {
            out[o++] = (char)((fr_hex_val(s[i + 1]) << 4) | fr_hex_val(s[i + 2]));
            i += 2;
        } else {
            out[o++] = s[i];
        }
    }
    *outlen = o;
    return out;
}

static void sb_pct_encode(SB *sb, const char *s, int64_t len, Charset cs) {
    static const char hex[] = "0123456789ABCDEF";
    for (int64_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (fr_is_safe(c, cs)) {
            sb_char(sb, (char)c);
        } else {
            sb_char(sb, '%');
            sb_char(sb, hex[(c >> 4) & 0xF]);
            sb_char(sb, hex[c & 0xF]);
        }
    }
}

static int64_t fr_find_char(const char *s, int64_t len, char c) {
    for (int64_t i = 0; i < len; i++) if (s[i] == c) return i;
    return -1;
}
static int64_t fr_find_char_last(const char *s, int64_t len, char c) {
    for (int64_t i = len - 1; i >= 0; i--) if (s[i] == c) return i;
    return -1;
}
static int fr_starts_with(const char *s, int64_t len, const char *prefix) {
    int64_t plen = (int64_t)strlen(prefix);
    return len >= plen && memcmp(s, prefix, (size_t)plen) == 0;
}
static int fr_eq(const char *s, int64_t len, const char *lit) {
    int64_t litlen = (int64_t)strlen(lit);
    return len == litlen && memcmp(s, lit, (size_t)litlen) == 0;
}

static int fr_is_hex40(const char *s, int64_t len) {
    if (len != 40) return 0;
    for (int64_t i = 0; i < len; i++) if (fr_hex_val(s[i]) < 0) return 0;
    return 1;
}

/* Parses an unsigned decimal integer (optional leading '+', no leading
 * '-', leading zeros allowed -- matches real Nix's underlying
 * `string2Int`, confirmed: "+3" -> 3, "03" -> 3). Returns 0 (and leaves
 * *out untouched) if `s` isn't a valid such integer -- callers treat
 * that as "field absent", matching real Nix silently dropping an
 * unparseable revCount/lastModified rather than erroring. */
static int fr_parse_uint(const char *s, int64_t len, int64_t *out) {
    int64_t i = 0;
    if (len > 0 && s[0] == '+') i = 1;
    if (i >= len) return 0;
    int64_t v = 0;
    for (; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10 + (s[i] - '0');
        if (v > LL_INT_MAX) return 0;
    }
    *out = v;
    return 1;
}

/* ---------------------------------------------------------------------
 * Query string: "&"-separated "key=value" pairs, both percent-decoded.
 * A piece with no '=' at all is dropped entirely (matches real Nix's
 * "dubious URI query ... missing equal sign, ignoring" warning,
 * confirmed live). Earlier duplicate keys win (first-match-wins,
 * std::map::emplace semantics, confirmed live with "?x=1&x=2"). */
typedef struct {
    char *key;
    int64_t keylen;
    char *val;
    int64_t vallen;
    int used;
} QParam;

static QParam *fr_parse_query(const char *s, int64_t len, int64_t *out_n) {
    QParam *out = len > 0 ? malloc((size_t)len * sizeof(QParam) / 2 + sizeof(QParam)) : NULL;
    int64_t n = 0;
    int64_t i = 0;
    while (i <= len) {
        int64_t amp = fr_find_char(s + i, len - i, '&');
        int64_t piece_len = amp >= 0 ? amp : len - i;
        int64_t eq = fr_find_char(s + i, piece_len, '=');
        if (eq >= 0) {
            int64_t klen, vlen;
            char *k = fr_pct_decode(s + i, eq, &klen);
            char *v = fr_pct_decode(s + i + eq + 1, piece_len - eq - 1, &vlen);
            out[n].key = k;
            out[n].keylen = klen;
            out[n].val = v;
            out[n].vallen = vlen;
            out[n].used = 0;
            n++;
        }
        if (amp < 0) break;
        i += amp + 1;
    }
    *out_n = n;
    return out;
}

/* First (unused-aware is NOT applied here -- callers mark `used`
 * themselves; this always returns the first match, duplicates are never
 * consulted, matching the "first wins" rule above). */
static QParam *fr_qp_find(QParam *qp, int64_t n, const char *key) {
    for (int64_t i = 0; i < n; i++) if (fr_eq(qp[i].key, qp[i].keylen, key)) return &qp[i];
    return NULL;
}
/* Marks every entry (first match AND any later duplicates) with this
 * key as used, so it's excluded from a "leftover" query. */
static void fr_qp_consume(QParam *qp, int64_t n, const char *key) {
    for (int64_t i = 0; i < n; i++) if (fr_eq(qp[i].key, qp[i].keylen, key)) qp[i].used = 1;
}

/* ---------------------------------------------------------------------
 * Output query-param list builder: collects (key,value) pairs, then
 * sorts alphabetically by key (confirmed: every serialized query string
 * in every fixture is key-sorted) and joins as "k1=v1&k2=v2...".
 * --------------------------------------------------------------------- */
typedef struct {
    const char *key; /* not owned */
    char *val; /* owned, freed by caller after use */
    int64_t vallen;
} QOut;

static void qout_add(QOut *arr, int *n, const char *key, const char *val, int64_t vallen) {
    arr[*n].key = key;
    arr[*n].val = malloc((size_t)vallen + 1);
    memcpy(arr[*n].val, val, (size_t)vallen);
    arr[*n].vallen = vallen;
    (*n)++;
}
static void qout_add_int(QOut *arr, int *n, const char *key, int64_t v) {
    char buf[32];
    int len = snprintf(buf, sizeof buf, "%lld", (long long)v);
    qout_add(arr, n, key, buf, len);
}

static void qout_sort(QOut *arr, int n) {
    for (int i = 1; i < n; i++) {
        int j = i;
        QOut tmp = arr[i];
        while (j > 0) {
            int64_t minlen = (int64_t)strlen(arr[j - 1].key) < (int64_t)strlen(tmp.key) ? (int64_t)strlen(arr[j - 1].key) : (int64_t)strlen(tmp.key);
            int cmp = minlen > 0 ? memcmp(arr[j - 1].key, tmp.key, (size_t)minlen) : 0;
            if (cmp == 0) cmp = (int)strlen(arr[j - 1].key) - (int)strlen(tmp.key);
            if (cmp <= 0) break;
            arr[j] = arr[j - 1];
            j--;
        }
        arr[j] = tmp;
    }
}
static void sb_append_query(SB *sb, QOut *arr, int n) {
    qout_sort(arr, n);
    if (n > 0) sb_char(sb, '?');
    for (int i = 0; i < n; i++) {
        if (i > 0) sb_char(sb, '&');
        sb_cstr(sb, arr[i].key);
        sb_char(sb, '=');
        sb_pct_encode(sb, arr[i].val, arr[i].vallen, CS_QUERY);
        free(arr[i].val);
    }
}

/* ---------------------------------------------------------------------
 * `rev` normalization used only by flakeRefToString for the
 * github/gitlab/git/indirect address forms: a 40-char hex string is
 * lowercased, a "sha1-<base64>" SRI-shaped string is decoded and
 * re-rendered as lowercase hex (confirmed: `convertHash`-adjacent --
 * `flakeRefToString { type="github"; ...; rev="sha1-ASNF...="; }`
 * produces the plain-hex form directly in the URL), anything else is
 * passed through byte for byte (plain non-hash-shaped strings like
 * path-type's `rev = "x"` or `"abc"` are NEVER normalized -- callers for
 * the "path" type must not call this). Returned buffer is malloc'd. */
static char *fr_normalize_rev(const char *s, int64_t len, int64_t *outlen) {
    if (fr_is_hex40(s, len)) {
        char *out = malloc(40);
        for (int i = 0; i < 40; i++) out[i] = (char)tolower((unsigned char)s[i]);
        *outlen = 40;
        return out;
    }
    if (len > 5 && memcmp(s, "sha1-", 5) == 0) {
        unsigned char raw[32];
        int64_t n = ll_base64_decode(s + 5, (size_t)(len - 5), raw, sizeof raw);
        if (n == 20) {
            static const char hex[] = "0123456789abcdef";
            char *out = malloc(40);
            for (int i = 0; i < 20; i++) {
                out[2 * i] = hex[(raw[i] >> 4) & 0xF];
                out[2 * i + 1] = hex[raw[i] & 0xF];
            }
            *outlen = 40;
            return out;
        }
    }
    char *out = malloc((size_t)len > 0 ? (size_t)len : 1);
    memcpy(out, s, (size_t)len);
    *outlen = len;
    return out;
}

/* "" is a documented real-Nix quirk: an empty `narHash` attr is rendered
 * as the SRI form of the all-zero sha256 "dummy" hash rather than
 * erroring -- confirmed directly: `flakeRefToString { type="github";
 * ...; narHash=""; }` -> "...?narHash=sha256-AAAA...A%3D" byte for byte.
 * Any other value is passed through as-is (this project does not
 * implement real Nix's full `Hash::parseAny`/SRI validation for narHash
 * -- a deliberate, narrower scope cut). */
static const char FR_DUMMY_SHA256_SRI[] = "sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
static void fr_narhash_or_dummy(const char *s, int64_t len, const char **out, int64_t *outlen) {
    if (len == 0) {
        *out = FR_DUMMY_SHA256_SRI;
        *outlen = (int64_t)strlen(FR_DUMMY_SHA256_SRI);
    } else {
        *out = s;
        *outlen = len;
    }
}

/* ---------------------------------------------------------------------
 * attrset read helpers for flakeRefToString: force + the "negative int
 * given for any flake ref attr" generic guard (confirmed live:
 * `flakeRefToString { type="github"; ...; repo = -1; }` errors
 * "negative value given for flake ref attr repo: -1" -- this check
 * fires for ANY attribute whose forced value happens to be a negative
 * int, regardless of whether that attribute is normally int-shaped;
 * matches eval-fail-flake-ref-to-string-negative-integer.nix, which
 * triggers it via `repo`, not revCount/lastModified). Returns 1 and
 * writes the forced value through `*out` if `name` is present, 0
 * otherwise (never fatal just for being absent -- callers decide
 * optional vs required). */
static int fr_find(Value attrs, const char *name, Value *out) {
    int64_t idx = ll_bshared_attrs_find(attrs, ll_intern(name, strlen(name)));
    if (idx < 0) return 0;
    Value v = ll_force_for_print(ll_bshared_attrs_value(attrs, idx));
    if (ll_tag_of(v) == LL_TAG_INT && ll_int_value(v) < 0) {
        fr_fatalf("flakeRefToString", "negative value given for flake ref attr %s: %lld", name,
                  (long long)ll_int_value(v));
    }
    *out = v;
    return 1;
}
static void fr_string_bytes(Value v, const char *name, const char **bytes, int64_t *len) {
    if (ll_tag_of(v) != LL_TAG_PTR) fr_fatalf("flakeRefToString", "flake ref attr '%s' is not a string", name);
    *bytes = ll_ptr_bytes(v, len);
}

/* ===========================================================================
 * parseFlakeRef
 * ======================================================================= */

/* Splits raw (still percent-escaped) bytes by literal '/' into segments,
 * filtering out EMPTY segments entirely -- confirmed live/by fixture:
 * "github:/a/b", "github:a//b", and the heavily-slashed upstream fixture
 * ("github://////owner%42/////repo%41///branch%43////") all behave as if
 * every run of consecutive '/' (leading, trailing, or internal) were a
 * single separator with nothing meaningful between. `count` caps how
 * many segments are collected (always plenty; path segment counts here
 * are always tiny). */
static int64_t fr_split_segments(const char *s, int64_t len, const char **seg, int64_t *seglen, int64_t maxn) {
    int64_t n = 0;
    int64_t i = 0;
    while (i < len && n < maxn) {
        if (s[i] == '/') { i++; continue; }
        int64_t start = i;
        while (i < len && s[i] != '/') i++;
        seg[n] = s + start;
        seglen[n] = i - start;
        n++;
    }
    return n;
}

static Value fr_decode_to_string(const char *s, int64_t len) {
    int64_t dlen;
    char *d = fr_pct_decode(s, len, &dlen);
    Value v = ll_make_string_value(d, (size_t)dlen);
    free(d);
    return v;
}

static Value fr_string_attr(const char *key, const char *bytes, int64_t len) {
    (void)key;
    return ll_make_string_value(bytes, (size_t)len);
}

/* Small fixed-capacity attr-list builder shared by every parse_* /
 * *_to_string function below -- avoids six near-identical "collect
 * syms/vals, call ll_bshared_make_attrs" blocks. */
typedef struct {
    uint32_t syms[16];
    Value vals[16];
    int64_t n;
} AttrBuilder;
static void ab_init(AttrBuilder *ab) { ab->n = 0; }
static void ab_put(AttrBuilder *ab, const char *name, Value v) {
    ab->syms[ab->n] = ll_intern(name, strlen(name));
    ab->vals[ab->n] = v;
    ab->n++;
}
static Value ab_finish(AttrBuilder *ab) { return ll_bshared_make_attrs(ab->syms, ab->vals, ab->n); }

/* ---- github:/gitlab: (and any other "owner/repo"-shaped forge) ------ */
static Value fr_parse_forge(const char *type, const char *rest, int64_t restlen) {
    int64_t qmark = fr_find_char(rest, restlen, '?');
    const char *path_part = rest;
    int64_t path_len = qmark >= 0 ? qmark : restlen;
    const char *query_part = qmark >= 0 ? rest + qmark + 1 : NULL;
    int64_t query_len = qmark >= 0 ? restlen - qmark - 1 : 0;

    const char *seg[16];
    int64_t seglen[16];
    int64_t n = fr_split_segments(path_part, path_len, seg, seglen, 16);
    if (n < 2) fr_fatalf("parseFlakeRef", "invalid %s flake reference (missing owner/repo)", type);

    Value owner = fr_decode_to_string(seg[0], seglen[0]);
    Value repo = fr_decode_to_string(seg[1], seglen[1]);

    int has_path_ref = 0, has_path_rev = 0;
    char *path_ref = NULL;
    int64_t path_ref_len = 0;
    char *path_rev = NULL;
    int64_t path_rev_len = 0;

    if (n == 3) {
        int64_t dlen;
        char *d = fr_pct_decode(seg[2], seglen[2], &dlen);
        if (fr_is_hex40(d, dlen)) { path_rev = d; path_rev_len = dlen; has_path_rev = 1; }
        else { path_ref = d; path_ref_len = dlen; has_path_ref = 1; }
    } else if (n > 3) {
        int64_t lastlen;
        char *last = fr_pct_decode(seg[n - 1], seglen[n - 1], &lastlen);
        SB joined;
        sb_init(&joined);
        int64_t upto = fr_is_hex40(last, lastlen) ? n - 1 : n;
        for (int64_t i = 2; i < upto; i++) {
            if (i > 2) sb_char(&joined, '/');
            int64_t dlen;
            char *d = fr_pct_decode(seg[i], seglen[i], &dlen);
            sb_bytes(&joined, d, dlen);
            free(d);
        }
        path_ref_len = joined.len;
        path_ref = malloc((size_t)path_ref_len > 0 ? (size_t)path_ref_len : 1);
        memcpy(path_ref, joined.buf, (size_t)path_ref_len);
        free(joined.buf);
        has_path_ref = 1;
        if (fr_is_hex40(last, lastlen)) { path_rev = last; path_rev_len = lastlen; has_path_rev = 1; }
        else free(last);
    }

    int64_t qn = 0;
    QParam *qp = query_part ? fr_parse_query(query_part, query_len, &qn) : NULL;
    QParam *q_ref = qp ? fr_qp_find(qp, qn, "ref") : NULL;
    QParam *q_rev = qp ? fr_qp_find(qp, qn, "rev") : NULL;
    QParam *q_dir = qp ? fr_qp_find(qp, qn, "dir") : NULL;
    QParam *q_host = qp ? fr_qp_find(qp, qn, "host") : NULL;
    QParam *q_narHash = qp ? fr_qp_find(qp, qn, "narHash") : NULL;
    if (qp) {
        for (int64_t i = 0; i < qn; i++) {
            if (&qp[i] == q_ref || &qp[i] == q_rev || &qp[i] == q_dir || &qp[i] == q_host || &qp[i] == q_narHash)
                continue;
            fr_fatalf("parseFlakeRef", "URL contains unknown parameter '%.*s'", (int)qp[i].keylen, qp[i].key);
        }
    }

    if (has_path_ref && q_ref) fr_fatal("parseFlakeRef", "flake reference specifies a ref both in the path and as a query parameter");
    if (has_path_rev && q_rev) fr_fatal("parseFlakeRef", "flake reference specifies a rev both in the path and as a query parameter");
    /* A ref AND a rev ending up BOTH set is only legitimate when they both
     * came from the SAME multi-segment path position (branch-then-commit,
     * e.g. "owner/repo/branch/<hex40>") -- any other combination (one
     * from the path and the other from the query, or both from the
     * query) is ambiguous and a hard error, confirmed by fixture:
     * eval-fail-parse-flake-ref-github-ref-and-rev.nix (query ref= AND
     * query rev=) and eval-fail-parse-flake-ref-github-path-ref-and-
     * rev.nix (path-derived ref, query rev=) both fail. */
    if ((has_path_ref || q_ref) && (has_path_rev || q_rev) && !(has_path_ref && has_path_rev))
        fr_fatal("parseFlakeRef", "flake reference names a branch/tag and a commit at the same time");

    AttrBuilder ab;
    ab_init(&ab);
    ab_put(&ab, "type", fr_string_attr("type", type, (int64_t)strlen(type)));
    ab_put(&ab, "owner", owner);
    ab_put(&ab, "repo", repo);
    if (has_path_ref) { ab_put(&ab, "ref", fr_string_attr("ref", path_ref, path_ref_len)); free(path_ref); }
    else if (q_ref) ab_put(&ab, "ref", fr_string_attr("ref", q_ref->val, q_ref->vallen));
    if (has_path_rev) { ab_put(&ab, "rev", fr_string_attr("rev", path_rev, path_rev_len)); free(path_rev); }
    else if (q_rev) ab_put(&ab, "rev", fr_string_attr("rev", q_rev->val, q_rev->vallen));
    if (q_dir) ab_put(&ab, "dir", fr_string_attr("dir", q_dir->val, q_dir->vallen));
    if (q_host) ab_put(&ab, "host", fr_string_attr("host", q_host->val, q_host->vallen));
    if (q_narHash) ab_put(&ab, "narHash", fr_string_attr("narHash", q_narHash->val, q_narHash->vallen));
    return ab_finish(&ab);
}

/* ---- bare flake id / "flake:" indirect ------------------------------- */
static int fr_is_id_char(char c, int first) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return 1;
    if (first) return 0;
    return (c >= '0' && c <= '9') || c == '_' || c == '-';
}
static int fr_is_valid_id(const char *s, int64_t len) {
    if (len == 0) return 0;
    if (!fr_is_id_char(s[0], 1)) return 0;
    for (int64_t i = 1; i < len; i++) if (!fr_is_id_char(s[i], 0)) return 0;
    return 1;
}

static Value fr_parse_indirect(const char *rest, int64_t restlen) {
    int64_t qmark = fr_find_char(rest, restlen, '?');
    const char *path_part = rest;
    int64_t path_len = qmark >= 0 ? qmark : restlen;
    const char *query_part = qmark >= 0 ? rest + qmark + 1 : NULL;
    int64_t query_len = qmark >= 0 ? restlen - qmark - 1 : 0;

    const char *seg[16];
    int64_t seglen[16];
    int64_t n = fr_split_segments(path_part, path_len, seg, seglen, 16);
    if (n == 0 || !fr_is_valid_id(seg[0], seglen[0]))
        fr_fatal("parseFlakeRef", "flake reference is not a valid indirect flake reference nor an absolute path");

    Value id = fr_string_attr("id", seg[0], seglen[0]);

    int has_ref = 0, has_rev = 0;
    char *ref_s = NULL;
    int64_t ref_len = 0;
    char *rev_s = NULL;
    int64_t rev_len = 0;

    if (n == 2) {
        int64_t dlen;
        char *d = fr_pct_decode(seg[1], seglen[1], &dlen);
        if (fr_is_hex40(d, dlen)) { rev_s = d; rev_len = dlen; has_rev = 1; }
        else { ref_s = d; ref_len = dlen; has_ref = 1; }
    } else if (n > 2) {
        int64_t lastlen;
        char *last = fr_pct_decode(seg[n - 1], seglen[n - 1], &lastlen);
        SB joined;
        sb_init(&joined);
        int64_t upto = fr_is_hex40(last, lastlen) ? n - 1 : n;
        for (int64_t i = 1; i < upto; i++) {
            if (i > 1) sb_char(&joined, '/');
            int64_t dlen;
            char *d = fr_pct_decode(seg[i], seglen[i], &dlen);
            sb_bytes(&joined, d, dlen);
            free(d);
        }
        ref_len = joined.len;
        ref_s = malloc((size_t)ref_len > 0 ? (size_t)ref_len : 1);
        memcpy(ref_s, joined.buf, (size_t)ref_len);
        free(joined.buf);
        has_ref = 1;
        if (fr_is_hex40(last, lastlen)) { rev_s = last; rev_len = lastlen; has_rev = 1; }
        else free(last);
    }
    /* Lowercase a hex-shaped rev (confirmed: "nixpkgs/ABCDEF0123..." parses
     * to rev = "abcdef0123..." -- indirect lowercases at PARSE time, unlike
     * github/git which only normalize at flakeRefToString time). */
    if (has_rev) for (int64_t i = 0; i < rev_len; i++) rev_s[i] = (char)tolower((unsigned char)rev_s[i]);

    int64_t qn = 0;
    QParam *qp = query_part ? fr_parse_query(query_part, query_len, &qn) : NULL;
    QParam *q_ref = qp ? fr_qp_find(qp, qn, "ref") : NULL;
    QParam *q_rev = qp ? fr_qp_find(qp, qn, "rev") : NULL;
    QParam *q_dir = qp ? fr_qp_find(qp, qn, "dir") : NULL;
    QParam *q_narHash = qp ? fr_qp_find(qp, qn, "narHash") : NULL;
    /* Unrecognized query keys are silently dropped for indirect --
     * confirmed live: `flake:nixpkgs?foo=1` -> { id = "nixpkgs"; } with
     * no error, unlike github/gitlab/path's strict allow-list. */

    AttrBuilder ab;
    ab_init(&ab);
    ab_put(&ab, "type", fr_string_attr("type", "indirect", 8));
    ab_put(&ab, "id", id);
    if (has_ref) { ab_put(&ab, "ref", fr_string_attr("ref", ref_s, ref_len)); free(ref_s); }
    else if (q_ref) ab_put(&ab, "ref", fr_string_attr("ref", q_ref->val, q_ref->vallen));
    if (has_rev) { ab_put(&ab, "rev", fr_string_attr("rev", rev_s, rev_len)); free(rev_s); }
    else if (q_rev) ab_put(&ab, "rev", fr_string_attr("rev", q_rev->val, q_rev->vallen));
    if (q_dir) ab_put(&ab, "dir", fr_string_attr("dir", q_dir->val, q_dir->vallen));
    if (q_narHash) ab_put(&ab, "narHash", fr_string_attr("narHash", q_narHash->val, q_narHash->vallen));
    return ab_finish(&ab);
}

/* ---- path: / bare absolute path -------------------------------------
 * `is_explicit`: 1 for the "path:" scheme prefix (percent-decodes the
 * path component, same as every other scheme), 0 for a bare absolute
 * path string (the path component is taken 100% literally/raw -- NOT
 * percent-decoded -- confirmed by two otherwise-identical fixture
 * entries: bare "/a%20b" keeps literal text "/a%20b", but "path:/a%20b"
 * decodes to "/a b". Only the query component is decoded in both forms.
 */
static char *fr_normalize_path_text(const char *s, int64_t len, int decode, int64_t *outlen) {
    int leading = len > 0 && s[0] == '/';
    int trailing = len > 1 && s[len - 1] == '/';
    const char *seg[64];
    int64_t seglen[64];
    int64_t n = fr_split_segments(s, len, seg, seglen, 64);
    SB sb;
    sb_init(&sb);
    if (n == 0) {
        if (leading) sb_char(&sb, '/');
    } else {
        if (leading) sb_char(&sb, '/');
        for (int64_t i = 0; i < n; i++) {
            if (i > 0) sb_char(&sb, '/');
            if (decode) {
                int64_t dlen;
                char *d = fr_pct_decode(seg[i], seglen[i], &dlen);
                sb_bytes(&sb, d, dlen);
                free(d);
            } else {
                sb_bytes(&sb, seg[i], seglen[i]);
            }
        }
        if (trailing) sb_char(&sb, '/');
    }
    *outlen = sb.len;
    char *out = malloc((size_t)sb.len > 0 ? (size_t)sb.len : 1);
    memcpy(out, sb.buf, (size_t)sb.len);
    free(sb.buf);
    return out;
}

static Value fr_parse_path(const char *rest, int64_t restlen, int is_explicit) {
    int64_t qmark = fr_find_char(rest, restlen, '?');
    const char *path_part = rest;
    int64_t path_len = qmark >= 0 ? qmark : restlen;
    const char *query_part = qmark >= 0 ? rest + qmark + 1 : NULL;
    int64_t query_len = qmark >= 0 ? restlen - qmark - 1 : 0;

    char *path_text;
    int64_t path_text_len;
    if (is_explicit && path_len == 0) {
        path_text = malloc(1);
        path_text[0] = '/';
        path_text_len = 1;
    } else {
        path_text = fr_normalize_path_text(path_part, path_len, is_explicit, &path_text_len);
    }

    int64_t qn = 0;
    QParam *qp = query_part ? fr_parse_query(query_part, query_len, &qn) : NULL;
    QParam *q_rev = qp ? fr_qp_find(qp, qn, "rev") : NULL;
    QParam *q_revCount = qp ? fr_qp_find(qp, qn, "revCount") : NULL;
    QParam *q_lastModified = qp ? fr_qp_find(qp, qn, "lastModified") : NULL;
    QParam *q_narHash = qp ? fr_qp_find(qp, qn, "narHash") : NULL;
    QParam *q_dir = qp ? fr_qp_find(qp, qn, "dir") : NULL;
    if (qp) {
        for (int64_t i = 0; i < qn; i++) {
            if (&qp[i] == q_rev || &qp[i] == q_revCount || &qp[i] == q_lastModified || &qp[i] == q_narHash ||
                &qp[i] == q_dir)
                continue;
            fr_fatalf("parseFlakeRef", "path URL has unsupported parameter '%.*s'", (int)qp[i].keylen, qp[i].key);
        }
    }

    AttrBuilder ab;
    ab_init(&ab);
    ab_put(&ab, "type", fr_string_attr("type", "path", 4));
    ab_put(&ab, "path", fr_string_attr("path", path_text, path_text_len));
    free(path_text);
    if (q_rev) ab_put(&ab, "rev", fr_string_attr("rev", q_rev->val, q_rev->vallen));
    int64_t iv;
    if (q_revCount && fr_parse_uint(q_revCount->val, q_revCount->vallen, &iv)) ab_put(&ab, "revCount", ll_make_int(iv));
    if (q_lastModified && fr_parse_uint(q_lastModified->val, q_lastModified->vallen, &iv)) ab_put(&ab, "lastModified", ll_make_int(iv));
    if (q_narHash) ab_put(&ab, "narHash", fr_string_attr("narHash", q_narHash->val, q_narHash->vallen));
    if (q_dir) ab_put(&ab, "dir", fr_string_attr("dir", q_dir->val, q_dir->vallen));
    return ab_finish(&ab);
}

/* ---- generic "scheme://[authority]path[?query]" URL (used by
 * git+/tarball+/file+ and bare git://,http(s)://,file:) ---------------
 * Authority's port is normalized here (leading zeros stripped, an empty
 * port dropped entirely -- confirmed live: "u:p@x.org:080/a" ->
 * "u:p@x.org:80/a", "x.org:/a" -> "x.org/a") since BOTH parseFlakeRef and
 * flakeRefToString need this, and parseFlakeRef needs it applied
 * directly to its own output (not just at re-serialization time). Path
 * segments are decoded then immediately re-encoded with CS_SEGMENT (so
 * an escaped '/' baked into one segment, e.g "%41%2f", round-trips back
 * through its decoded form "A/" to "A%2F" -- confirmed live with
 * "https://x.org/%41%2f?%41=%42" -> url "https://x.org/A%2F?A=B"). */
typedef struct {
    const char *scheme;
    int64_t scheme_len;
    int has_authority;
    char *authority; /* malloc'd, port-normalized */
    int64_t authority_len;
    char *path; /* malloc'd, segment-normalized */
    int64_t path_len;
    QParam *query;
    int64_t query_n;
} ParsedUrl;

static char *fr_normalize_authority(const char *s, int64_t len, int64_t *outlen) {
    int64_t at = fr_find_char_last(s, len, '@');
    const char *userinfo = at >= 0 ? s : NULL;
    int64_t userinfo_len = at >= 0 ? at + 1 : 0;
    const char *hostport = at >= 0 ? s + at + 1 : s;
    int64_t hostport_len = at >= 0 ? len - at - 1 : len;
    int64_t colon = fr_find_char_last(hostport, hostport_len, ':');
    SB sb;
    sb_init(&sb);
    if (userinfo) sb_bytes(&sb, userinfo, userinfo_len);
    if (colon >= 0) {
        sb_bytes(&sb, hostport, colon);
        const char *port = hostport + colon + 1;
        int64_t portlen = hostport_len - colon - 1;
        int64_t pv;
        if (portlen > 0 && fr_parse_uint(port, portlen, &pv)) {
            sb_char(&sb, ':');
            char buf[32];
            int blen = snprintf(buf, sizeof buf, "%lld", (long long)pv);
            sb_bytes(&sb, buf, blen);
        } else if (portlen > 0) {
            sb_char(&sb, ':');
            sb_bytes(&sb, port, portlen);
        } /* empty port: drop the colon entirely */
    } else {
        sb_bytes(&sb, hostport, hostport_len);
    }
    *outlen = sb.len;
    char *out = malloc((size_t)sb.len > 0 ? (size_t)sb.len : 1);
    memcpy(out, sb.buf, (size_t)sb.len);
    free(sb.buf);
    return out;
}

static char *fr_normalize_url_path(const char *s, int64_t len, int64_t *outlen) {
    const char *seg[256];
    int64_t seglen[256];
    int64_t n = 0;
    int64_t i = 0;
    /* Unlike fr_split_segments (github/indirect/path), this keeps EMPTY
     * segments (a genuine "//" in a URL path is preserved, not
     * collapsed -- only the "path" type's own bespoke grammar collapses
     * doubled slashes). */
    int64_t start = 0;
    while (i <= len && n < 256) {
        if (i == len || s[i] == '/') {
            seg[n] = s + start;
            seglen[n] = i - start;
            n++;
            start = i + 1;
        }
        i++;
    }
    SB sb;
    sb_init(&sb);
    for (int64_t k = 0; k < n; k++) {
        if (k > 0) sb_char(&sb, '/');
        int64_t dlen;
        char *d = fr_pct_decode(seg[k], seglen[k], &dlen);
        sb_pct_encode(&sb, d, dlen, CS_SEGMENT);
        free(d);
    }
    *outlen = sb.len;
    char *out = malloc((size_t)sb.len > 0 ? (size_t)sb.len : 1);
    memcpy(out, sb.buf, (size_t)sb.len);
    free(sb.buf);
    return out;
}

static void fr_parse_generic_url(const char *s, int64_t len, ParsedUrl *out) {
    int64_t colon = fr_find_char(s, len, ':');
    if (colon < 0) fr_fatal("parseFlakeRef", "URL has no scheme");
    out->scheme = s;
    out->scheme_len = colon;
    const char *rest = s + colon + 1;
    int64_t restlen = len - colon - 1;
    if (restlen >= 2 && rest[0] == '/' && rest[1] == '/') {
        rest += 2;
        restlen -= 2;
        int64_t i = 0;
        while (i < restlen && rest[i] != '/' && rest[i] != '?') i++;
        out->has_authority = 1;
        out->authority = fr_normalize_authority(rest, i, &out->authority_len);
        rest += i;
        restlen -= i;
    } else {
        out->has_authority = 0;
        out->authority = NULL;
        out->authority_len = 0;
    }
    int64_t qmark = fr_find_char(rest, restlen, '?');
    const char *path_raw = rest;
    int64_t path_raw_len = qmark >= 0 ? qmark : restlen;
    const char *query_raw = qmark >= 0 ? rest + qmark + 1 : NULL;
    int64_t query_raw_len = qmark >= 0 ? restlen - qmark - 1 : 0;
    out->path = fr_normalize_url_path(path_raw, path_raw_len, &out->path_len);
    if (query_raw) out->query = fr_parse_query(query_raw, query_raw_len, &out->query_n);
    else { out->query = NULL; out->query_n = 0; }
}

static Value fr_rebuild_url(const char *scheme, int64_t scheme_len, int has_authority, const char *authority,
                             int64_t authority_len, const char *path, int64_t path_len, QOut *extra_query,
                             int extra_n) {
    SB sb;
    sb_init(&sb);
    sb_bytes(&sb, scheme, scheme_len);
    sb_char(&sb, ':');
    if (has_authority) {
        sb_cstr(&sb, "//");
        sb_bytes(&sb, authority, authority_len);
    }
    sb_bytes(&sb, path, path_len);
    sb_append_query(&sb, extra_query, extra_n);
    return sb_finish(&sb);
}

/* Builds the "leftover" query (every param not already consumed by a
 * recognized-key extraction) for git/tarball/file's own `url` field.
 * Two extra rules, both confirmed live/by fixture:
 *   - a literal "url" query key is always dropped outright (confirmed:
 *     `parseFlakeRef "https://x.org/a.tar.gz?dir=d&url=u"` keeps `dir`
 *     but drops `url=u` entirely -- not an error, not kept anywhere).
 *   - duplicate keys: first occurrence wins, later ones are dropped
 *     (same "emplace" semantics as fr_qp_find/the recognized-key path,
 *     confirmed: "?&&x=1&x=2" -> leftover "x=1" only, not "x=1&x=2"). */
static int qout_has_key(QOut *arr, int n, const char *key, int64_t keylen) {
    for (int i = 0; i < n; i++) if ((int64_t)strlen(arr[i].key) == keylen && memcmp(arr[i].key, key, (size_t)keylen) == 0) return 1;
    return 0;
}
/* Adds (key,val) only if `arr` doesn't already have that key -- used by
 * flakeRefToString's git/tarball `url`-merging: an already-present query
 * param embedded directly in the user-given `url` string wins over the
 * same-named top-level attr, confirmed live/by fixture
 * (eval-okay-flake-ref-to-string-schemes.nix's tarball `dir`-conflict
 * entry: url's own "dir=y" survives, the top-level `dir = "x"` attr is
 * silently dropped). */
static void qout_add_if_absent(QOut *arr, int *n, const char *key, const char *val, int64_t vallen) {
    if (qout_has_key(arr, *n, key, (int64_t)strlen(key))) return;
    qout_add(arr, n, key, val, vallen);
}
static void fr_build_leftover(QParam *qp, int64_t qn, QOut *leftover, int *ln) {
    for (int64_t i = 0; i < qn; i++) {
        if (qp[i].used) continue;
        if (fr_eq(qp[i].key, qp[i].keylen, "url")) continue;
        if (qout_has_key(leftover, *ln, qp[i].key, qp[i].keylen)) continue;
        char *keycopy = malloc((size_t)qp[i].keylen + 1);
        memcpy(keycopy, qp[i].key, (size_t)qp[i].keylen);
        keycopy[qp[i].keylen] = '\0';
        qout_add(leftover, ln, keycopy, qp[i].val, qp[i].vallen);
    }
}

/* ---- git+/git:// ------------------------------------------------------ */
static Value fr_parse_git(const char *url_body, int64_t url_body_len) {
    ParsedUrl pu;
    fr_parse_generic_url(url_body, url_body_len, &pu);

    QParam *q_ref = pu.query ? fr_qp_find(pu.query, pu.query_n, "ref") : NULL;
    QParam *q_rev = pu.query ? fr_qp_find(pu.query, pu.query_n, "rev") : NULL;
    QParam *q_shallow = pu.query ? fr_qp_find(pu.query, pu.query_n, "shallow") : NULL;
    QParam *q_submodules = pu.query ? fr_qp_find(pu.query, pu.query_n, "submodules") : NULL;
    QParam *q_lfs = pu.query ? fr_qp_find(pu.query, pu.query_n, "lfs") : NULL;
    QParam *q_exportIgnore = pu.query ? fr_qp_find(pu.query, pu.query_n, "exportIgnore") : NULL;
    QParam *q_allRefs = pu.query ? fr_qp_find(pu.query, pu.query_n, "allRefs") : NULL;
    QParam *q_dir = pu.query ? fr_qp_find(pu.query, pu.query_n, "dir") : NULL;
    if (q_ref) fr_qp_consume(pu.query, pu.query_n, "ref");
    if (q_rev) fr_qp_consume(pu.query, pu.query_n, "rev");
    if (q_shallow) fr_qp_consume(pu.query, pu.query_n, "shallow");
    if (q_submodules) fr_qp_consume(pu.query, pu.query_n, "submodules");
    if (q_lfs) fr_qp_consume(pu.query, pu.query_n, "lfs");
    if (q_exportIgnore) fr_qp_consume(pu.query, pu.query_n, "exportIgnore");
    if (q_allRefs) fr_qp_consume(pu.query, pu.query_n, "allRefs");
    if (q_dir) fr_qp_consume(pu.query, pu.query_n, "dir");

    QOut leftover[64];
    int ln = 0;
    fr_build_leftover(pu.query, pu.query_n, leftover, &ln);
    Value url_val = fr_rebuild_url(pu.scheme, pu.scheme_len, pu.has_authority, pu.authority, pu.authority_len,
                                    pu.path, pu.path_len, leftover, ln);

    AttrBuilder ab;
    ab_init(&ab);
    ab_put(&ab, "type", fr_string_attr("type", "git", 3));
    ab_put(&ab, "url", url_val);
    if (q_ref) ab_put(&ab, "ref", fr_string_attr("ref", q_ref->val, q_ref->vallen));
    if (q_rev) ab_put(&ab, "rev", fr_string_attr("rev", q_rev->val, q_rev->vallen));
    if (q_shallow) ab_put(&ab, "shallow", fr_eq(q_shallow->val, q_shallow->vallen, "1") ? LL_TRUE : LL_FALSE);
    if (q_submodules) ab_put(&ab, "submodules", fr_eq(q_submodules->val, q_submodules->vallen, "1") ? LL_TRUE : LL_FALSE);
    if (q_lfs) ab_put(&ab, "lfs", fr_eq(q_lfs->val, q_lfs->vallen, "1") ? LL_TRUE : LL_FALSE);
    if (q_exportIgnore) ab_put(&ab, "exportIgnore", fr_eq(q_exportIgnore->val, q_exportIgnore->vallen, "1") ? LL_TRUE : LL_FALSE);
    if (q_allRefs) ab_put(&ab, "allRefs", fr_eq(q_allRefs->val, q_allRefs->vallen, "1") ? LL_TRUE : LL_FALSE);
    if (q_dir) ab_put(&ab, "dir", fr_string_attr("dir", q_dir->val, q_dir->vallen));
    return ab_finish(&ab);
}

/* ---- tarball+/file+ / bare http(s):// / bare file: -------------------
 * `unpack` and `name` are both parsed (so neither is ever mistaken for
 * an "unknown parameter") but never stored as an attribute for either
 * type -- confirmed by fixture: eval-okay-parse-flake-ref-tarball.nix's
 * "...?name=x&unpack=1&narHash=..." entry has neither `name` nor
 * `unpack` in its expected attrset, and flakeRefToString of a hand-built
 * `{type="file";...;unpack=true;}` drops `unpack` too (see
 * fr_file_to_string). */
static Value fr_parse_tarball_or_file(const char *type, const char *url_body, int64_t url_body_len) {
    ParsedUrl pu;
    fr_parse_generic_url(url_body, url_body_len, &pu);

    QParam *q_rev = pu.query ? fr_qp_find(pu.query, pu.query_n, "rev") : NULL;
    QParam *q_revCount = pu.query ? fr_qp_find(pu.query, pu.query_n, "revCount") : NULL;
    QParam *q_lastModified = pu.query ? fr_qp_find(pu.query, pu.query_n, "lastModified") : NULL;
    QParam *q_narHash = pu.query ? fr_qp_find(pu.query, pu.query_n, "narHash") : NULL;
    QParam *q_dir = pu.query ? fr_qp_find(pu.query, pu.query_n, "dir") : NULL;
    QParam *q_name = pu.query ? fr_qp_find(pu.query, pu.query_n, "name") : NULL;
    QParam *q_unpack = pu.query ? fr_qp_find(pu.query, pu.query_n, "unpack") : NULL;
    if (q_rev) fr_qp_consume(pu.query, pu.query_n, "rev");
    if (q_revCount) fr_qp_consume(pu.query, pu.query_n, "revCount");
    if (q_lastModified) fr_qp_consume(pu.query, pu.query_n, "lastModified");
    if (q_narHash) fr_qp_consume(pu.query, pu.query_n, "narHash");
    if (q_dir) fr_qp_consume(pu.query, pu.query_n, "dir");
    if (q_name) fr_qp_consume(pu.query, pu.query_n, "name");
    if (q_unpack) fr_qp_consume(pu.query, pu.query_n, "unpack");

    QOut leftover[64];
    int ln = 0;
    fr_build_leftover(pu.query, pu.query_n, leftover, &ln);
    Value url_val = fr_rebuild_url(pu.scheme, pu.scheme_len, pu.has_authority, pu.authority, pu.authority_len,
                                    pu.path, pu.path_len, leftover, ln);

    AttrBuilder ab;
    ab_init(&ab);
    ab_put(&ab, "type", fr_string_attr("type", type, (int64_t)strlen(type)));
    ab_put(&ab, "url", url_val);
    if (q_rev) ab_put(&ab, "rev", fr_string_attr("rev", q_rev->val, q_rev->vallen));
    int64_t iv;
    if (q_revCount && fr_parse_uint(q_revCount->val, q_revCount->vallen, &iv)) ab_put(&ab, "revCount", ll_make_int(iv));
    if (q_lastModified && fr_parse_uint(q_lastModified->val, q_lastModified->vallen, &iv)) ab_put(&ab, "lastModified", ll_make_int(iv));
    if (q_narHash) ab_put(&ab, "narHash", fr_string_attr("narHash", q_narHash->val, q_narHash->vallen));
    if (q_dir) ab_put(&ab, "dir", fr_string_attr("dir", q_dir->val, q_dir->vallen));
    /* `name` is recognized (so it's never misreported as an "unknown
     * parameter") but never stored, same as `unpack` -- confirmed by
     * fixture: eval-okay-parse-flake-ref-tarball.nix's own
     * "tarball+https://example.com/a?name=x&unpack=1&narHash=..." entry
     * has no `name` in its expected attrset either. */
    (void)q_name;
    return ab_finish(&ab);
}

static Value prim_parseFlakeRef(const Value *args, int64_t argc) {
    (void)argc;
    LL_REQUIRE_STRING(args[0], "parseFlakeRef");
    int64_t len;
    const char *bytes = ll_ptr_bytes(args[0], &len);

    /* Strip a trailing fragment globally -- an EMPTY fragment ("...#")
     * is silently dropped (confirmed: "nixpkgs#", "tarball+http://.../a#"
     * both succeed), a non-empty one is a hard error for every scheme
     * (confirmed: "github:NixOS/nixpkgs#hello" fails). */
    int64_t hash = fr_find_char(bytes, len, '#');
    if (hash >= 0) {
        if (hash + 1 < len) fr_fatal("parseFlakeRef", "unexpected fragment in flake reference");
        len = hash;
    }

    if (fr_starts_with(bytes, len, "github:")) return fr_parse_forge("github", bytes + 7, len - 7);
    if (fr_starts_with(bytes, len, "gitlab:")) return fr_parse_forge("gitlab", bytes + 7, len - 7);
    if (fr_starts_with(bytes, len, "sourcehut:")) return fr_parse_forge("sourcehut", bytes + 10, len - 10);
    if (fr_starts_with(bytes, len, "flake:")) return fr_parse_indirect(bytes + 6, len - 6);
    if (fr_starts_with(bytes, len, "git+")) return fr_parse_git(bytes + 4, len - 4);
    if (fr_starts_with(bytes, len, "tarball+")) return fr_parse_tarball_or_file("tarball", bytes + 8, len - 8);
    if (fr_starts_with(bytes, len, "file+")) return fr_parse_tarball_or_file("file", bytes + 5, len - 5);
    if (fr_starts_with(bytes, len, "path:")) return fr_parse_path(bytes + 5, len - 5, 1);
    if (fr_starts_with(bytes, len, "git://")) return fr_parse_git(bytes, len);
    if (fr_starts_with(bytes, len, "http://") || fr_starts_with(bytes, len, "https://"))
        return fr_parse_tarball_or_file("tarball", bytes, len);
    if (fr_starts_with(bytes, len, "file:")) return fr_parse_tarball_or_file("tarball", bytes, len);
    if (len > 0 && bytes[0] == '/') return fr_parse_path(bytes, len, 0);
    return fr_parse_indirect(bytes, len);
}

/* ===========================================================================
 * flakeRefToString
 * ======================================================================= */

static void sb_segment(SB *sb, const char *s, int64_t len) { sb_pct_encode(sb, s, len, CS_SEGMENT); }

static Value fr_forge_to_string(const char *type, Value attrs) {
    Value owner_v, repo_v;
    if (!fr_find(attrs, "owner", &owner_v)) fr_fatalf("flakeRefToString", "%s flake reference is missing 'owner'", type);
    if (!fr_find(attrs, "repo", &repo_v)) fr_fatalf("flakeRefToString", "%s flake reference is missing 'repo'", type);
    const char *owner_b, *repo_b;
    int64_t owner_l, repo_l;
    fr_string_bytes(owner_v, "owner", &owner_b, &owner_l);
    fr_string_bytes(repo_v, "repo", &repo_b, &repo_l);

    Value ref_v, rev_v, dir_v, host_v, narHash_v;
    int has_ref = fr_find(attrs, "ref", &ref_v);
    int has_rev = fr_find(attrs, "rev", &rev_v);
    if (has_ref && has_rev)
        fr_fatal("flakeRefToString", "flake reference names a branch or tag, or a commit, not both");
    int has_dir = fr_find(attrs, "dir", &dir_v);
    int has_host = fr_find(attrs, "host", &host_v);
    int has_narHash = fr_find(attrs, "narHash", &narHash_v);

    SB sb;
    sb_init(&sb);
    sb_cstr(&sb, type);
    sb_char(&sb, ':');
    sb_segment(&sb, owner_b, owner_l);
    sb_char(&sb, '/');
    sb_segment(&sb, repo_b, repo_l);
    if (has_rev) {
        const char *rb;
        int64_t rl;
        fr_string_bytes(rev_v, "rev", &rb, &rl);
        int64_t nlen;
        char *norm = fr_normalize_rev(rb, rl, &nlen);
        sb_char(&sb, '/');
        sb_segment(&sb, norm, nlen);
        free(norm);
    } else if (has_ref) {
        const char *rb;
        int64_t rl;
        fr_string_bytes(ref_v, "ref", &rb, &rl);
        sb_char(&sb, '/');
        sb_segment(&sb, rb, rl);
    }

    QOut q[8];
    int qn = 0;
    if (has_dir) {
        const char *b;
        int64_t l;
        fr_string_bytes(dir_v, "dir", &b, &l);
        qout_add(q, &qn, "dir", b, l);
    }
    if (has_host) {
        const char *b;
        int64_t l;
        fr_string_bytes(host_v, "host", &b, &l);
        qout_add(q, &qn, "host", b, l);
    }
    if (has_narHash) {
        const char *b;
        int64_t l;
        fr_string_bytes(narHash_v, "narHash", &b, &l);
        const char *nb;
        int64_t nl;
        fr_narhash_or_dummy(b, l, &nb, &nl);
        qout_add(q, &qn, "narHash", nb, nl);
    }
    sb_append_query(&sb, q, qn);
    return sb_finish(&sb);
}

static Value fr_indirect_to_string(Value attrs) {
    Value id_v;
    if (!fr_find(attrs, "id", &id_v)) fr_fatal("flakeRefToString", "indirect flake reference is missing 'id'");
    const char *id_b;
    int64_t id_l;
    fr_string_bytes(id_v, "id", &id_b, &id_l);

    Value ref_v, rev_v, dir_v, narHash_v;
    int has_ref = fr_find(attrs, "ref", &ref_v);
    int has_rev = fr_find(attrs, "rev", &rev_v);
    int has_dir = fr_find(attrs, "dir", &dir_v);
    int has_narHash = fr_find(attrs, "narHash", &narHash_v);
    (void)has_narHash; /* dropped at toString time, confirmed live -- only read for the negative-int guard */

    SB sb;
    sb_init(&sb);
    sb_cstr(&sb, "flake:");
    sb_segment(&sb, id_b, id_l);
    if (has_ref) {
        const char *b;
        int64_t l;
        fr_string_bytes(ref_v, "ref", &b, &l);
        sb_char(&sb, '/');
        sb_segment(&sb, b, l);
    }
    if (has_rev) {
        const char *b;
        int64_t l;
        fr_string_bytes(rev_v, "rev", &b, &l);
        int64_t nlen;
        char *norm = fr_normalize_rev(b, l, &nlen);
        sb_char(&sb, '/');
        sb_segment(&sb, norm, nlen);
        free(norm);
    }
    QOut q[4];
    int qn = 0;
    if (has_dir) {
        const char *b;
        int64_t l;
        fr_string_bytes(dir_v, "dir", &b, &l);
        qout_add(q, &qn, "dir", b, l);
    }
    sb_append_query(&sb, q, qn);
    return sb_finish(&sb);
}

static Value fr_path_to_string(Value attrs) {
    Value path_v;
    if (!fr_find(attrs, "path", &path_v)) fr_fatal("flakeRefToString", "path flake reference is missing 'path'");
    const char *path_b;
    int64_t path_l;
    fr_string_bytes(path_v, "path", &path_b, &path_l);

    SB sb;
    sb_init(&sb);
    sb_cstr(&sb, "path:");
    sb_pct_encode(&sb, path_b, path_l, CS_QPATH);

    QOut q[8];
    int qn = 0;
    Value v;
    if (fr_find(attrs, "dir", &v)) {
        const char *b;
        int64_t l;
        fr_string_bytes(v, "dir", &b, &l);
        qout_add(q, &qn, "dir", b, l);
    }
    if (fr_find(attrs, "lastModified", &v)) {
        if (ll_tag_of(v) != LL_TAG_INT) fr_fatal("flakeRefToString", "'lastModified' must be an integer");
        qout_add_int(q, &qn, "lastModified", ll_int_value(v));
    }
    if (fr_find(attrs, "narHash", &v)) {
        const char *b;
        int64_t l;
        fr_string_bytes(v, "narHash", &b, &l);
        qout_add(q, &qn, "narHash", b, l); /* path type does NOT substitute the dummy hash for "" -- confirmed live */
    }
    if (fr_find(attrs, "rev", &v)) {
        const char *b;
        int64_t l;
        fr_string_bytes(v, "rev", &b, &l);
        qout_add(q, &qn, "rev", b, l);
    }
    if (fr_find(attrs, "revCount", &v)) {
        if (ll_tag_of(v) != LL_TAG_INT) fr_fatal("flakeRefToString", "'revCount' must be an integer");
        qout_add_int(q, &qn, "revCount", ll_int_value(v));
    }
    sb_append_query(&sb, q, qn);
    return sb_finish(&sb);
}

/* Strips a leading "git+"/"tarball+"/"file+" composite scheme prefix off
 * a `url` attribute's own raw text, if present (confirmed live with a
 * hand-built `url = "git+https://x.org/a?x=1";` -- flakeRefToString must
 * not double it up to "git+git+https://..."). */
static void fr_strip_composite_prefix(const char *s, int64_t len, const char **out, int64_t *outlen) {
    if (fr_starts_with(s, len, "git+")) { *out = s + 4; *outlen = len - 4; return; }
    if (fr_starts_with(s, len, "tarball+")) { *out = s + 8; *outlen = len - 8; return; }
    if (fr_starts_with(s, len, "file+")) { *out = s + 5; *outlen = len - 5; return; }
    *out = s;
    *outlen = len;
}

static Value fr_git_to_string(Value attrs) {
    Value url_v;
    if (!fr_find(attrs, "url", &url_v)) fr_fatal("flakeRefToString", "git flake reference is missing 'url'");
    const char *url_b;
    int64_t url_l;
    fr_string_bytes(url_v, "url", &url_b, &url_l);
    const char *body;
    int64_t bodylen;
    fr_strip_composite_prefix(url_b, url_l, &body, &bodylen);

    ParsedUrl pu;
    fr_parse_generic_url(body, bodylen, &pu);

    QOut q[64];
    int qn = 0;
    fr_build_leftover(pu.query, pu.query_n, q, &qn);
    /* `extra` (ref/rev/shallow/.../dir) is added only if the url's own
     * embedded query doesn't already have that key -- see
     * qout_add_if_absent's comment. */
    Value v;
    const char *b;
    int64_t l;
    if (fr_find(attrs, "ref", &v)) {
        fr_string_bytes(v, "ref", &b, &l);
        qout_add_if_absent(q, &qn, "ref", b, l);
    }
    if (fr_find(attrs, "rev", &v)) {
        fr_string_bytes(v, "rev", &b, &l);
        int64_t nlen;
        char *norm = fr_normalize_rev(b, l, &nlen);
        qout_add_if_absent(q, &qn, "rev", norm, nlen);
        free(norm);
    }
    static const char *const bool_keys[] = { "shallow", "submodules", "lfs", "exportIgnore" };
    for (int bi = 0; bi < 4; bi++) {
        if (fr_find(attrs, bool_keys[bi], &v)) {
            if (ll_tag_of(v) == LL_TAG_IMM && ll_imm_subtag(v) == LL_IMM_TRUE) qout_add_if_absent(q, &qn, bool_keys[bi], "1", 1);
        }
    }
    if (fr_find(attrs, "dir", &v)) {
        fr_string_bytes(v, "dir", &b, &l);
        qout_add_if_absent(q, &qn, "dir", b, l);
    }

    const char *prefix = (pu.scheme_len == 3 && memcmp(pu.scheme, "git", 3) == 0) ? "" : "git+";
    SB sb;
    sb_init(&sb);
    sb_cstr(&sb, prefix);
    sb_bytes(&sb, pu.scheme, pu.scheme_len);
    sb_char(&sb, ':');
    if (pu.has_authority) {
        sb_cstr(&sb, "//");
        sb_bytes(&sb, pu.authority, pu.authority_len);
    }
    sb_bytes(&sb, pu.path, pu.path_len);
    sb_append_query(&sb, q, qn);
    return sb_finish(&sb);
}

static Value fr_tarball_to_string(Value attrs) {
    Value url_v;
    if (!fr_find(attrs, "url", &url_v)) fr_fatal("flakeRefToString", "tarball flake reference is missing 'url'");
    const char *url_b;
    int64_t url_l;
    fr_string_bytes(url_v, "url", &url_b, &url_l);
    const char *body;
    int64_t bodylen;
    fr_strip_composite_prefix(url_b, url_l, &body, &bodylen);

    ParsedUrl pu;
    fr_parse_generic_url(body, bodylen, &pu);

    QOut q[64];
    int qn = 0;
    fr_build_leftover(pu.query, pu.query_n, q, &qn);
    Value v;
    if (fr_find(attrs, "narHash", &v)) {
        const char *b;
        int64_t l;
        fr_string_bytes(v, "narHash", &b, &l);
        const char *nb;
        int64_t nl;
        fr_narhash_or_dummy(b, l, &nb, &nl);
        qout_add_if_absent(q, &qn, "narHash", nb, nl);
    }
    if (fr_find(attrs, "dir", &v)) {
        const char *b;
        int64_t l;
        fr_string_bytes(v, "dir", &b, &l);
        qout_add_if_absent(q, &qn, "dir", b, l);
    }
    /* rev/revCount/lastModified/name are never re-added here -- confirmed
     * by fixture (eval-okay-flake-ref-to-string-schemes.nix's tarball
     * entry gives top-level lastModified/name but neither survives). */

    int bare = pu.scheme_len == 4 && memcmp(pu.scheme, "http", 4) == 0;
    bare = bare || (pu.scheme_len == 5 && memcmp(pu.scheme, "https", 5) == 0);
    bare = bare || (pu.scheme_len == 4 && memcmp(pu.scheme, "file", 4) == 0);
    const char *prefix = bare ? "" : "tarball+";
    SB sb;
    sb_init(&sb);
    sb_cstr(&sb, prefix);
    sb_bytes(&sb, pu.scheme, pu.scheme_len);
    sb_char(&sb, ':');
    if (pu.has_authority) {
        sb_cstr(&sb, "//");
        sb_bytes(&sb, pu.authority, pu.authority_len);
    }
    sb_bytes(&sb, pu.path, pu.path_len);
    sb_append_query(&sb, q, qn);
    return sb_finish(&sb);
}

/* FILE type's toString is a documented real-Nix quirk, confirmed by TWO
 * independent fixtures (eval-okay-flake-ref-to-string-schemes.nix's last
 * entry, and eval-okay-flake-ref-round-trip.nix's "file+https://..."
 * entry, which is same=false precisely because of this): it NEVER adds a
 * "file+" prefix and NEVER merges any top-level attr back in -- the
 * output is simply the (prefix-stripped, scheme/authority/path-
 * normalized) `url` field, verbatim. This is lossy (re-parsing the
 * result gives type="tarball", not "file") but matches real Nix exactly,
 * so it is NOT treated as a bug here. */
static Value fr_file_to_string(Value attrs) {
    Value url_v;
    if (!fr_find(attrs, "url", &url_v)) fr_fatal("flakeRefToString", "file flake reference is missing 'url'");
    const char *url_b;
    int64_t url_l;
    fr_string_bytes(url_v, "url", &url_b, &url_l);
    const char *body;
    int64_t bodylen;
    fr_strip_composite_prefix(url_b, url_l, &body, &bodylen);

    ParsedUrl pu;
    fr_parse_generic_url(body, bodylen, &pu);

    QOut q[64];
    int qn = 0;
    fr_build_leftover(pu.query, pu.query_n, q, &qn);
    return fr_rebuild_url(pu.scheme, pu.scheme_len, pu.has_authority, pu.authority, pu.authority_len, pu.path,
                           pu.path_len, q, qn);
}

static Value prim_flakeRefToString(const Value *args, int64_t argc) {
    (void)argc;
    Value attrs = ll_force_for_print(args[0]);
    LL_REQUIRE_ATTRS(attrs, "flakeRefToString");
    Value type_v;
    if (!fr_find(attrs, "type", &type_v)) fr_fatal("flakeRefToString", "flake reference has no 'type' attribute");
    const char *tb;
    int64_t tl;
    fr_string_bytes(type_v, "type", &tb, &tl);

    if (fr_eq(tb, tl, "github") || fr_eq(tb, tl, "gitlab") || fr_eq(tb, tl, "sourcehut")) {
        char tname[16];
        memcpy(tname, tb, (size_t)tl);
        tname[tl] = '\0';
        return fr_forge_to_string(tname, attrs);
    }
    if (fr_eq(tb, tl, "indirect")) return fr_indirect_to_string(attrs);
    if (fr_eq(tb, tl, "path")) return fr_path_to_string(attrs);
    if (fr_eq(tb, tl, "git")) return fr_git_to_string(attrs);
    if (fr_eq(tb, tl, "tarball")) return fr_tarball_to_string(attrs);
    if (fr_eq(tb, tl, "file")) return fr_file_to_string(attrs);
    fr_fatalf("flakeRefToString", "unsupported flake ref type '%.*s'", (int)tl, tb);
}

static Value build_parseFlakeRef(void) { return ll_bshared_make_primop((void *)prim_parseFlakeRef, 1); }
static Value build_flakeRefToString(void) { return ll_bshared_make_primop((void *)prim_flakeRefToString, 1); }

const LlBuiltinFlakerefDef LL_FLAKEREF_BUILTINS[] = {
    { "parseFlakeRef", build_parseFlakeRef },
    { "flakeRefToString", build_flakeRefToString },
};
const size_t LL_FLAKEREF_BUILTINS_COUNT = sizeof(LL_FLAKEREF_BUILTINS) / sizeof(LL_FLAKEREF_BUILTINS[0]);
