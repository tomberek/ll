/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
/*
 * builtins.fromTOML -- a scoped, hand-written recursive-descent TOML
 * parser modeled on this project's existing lexer.c/parser.c style
 * (plain index-into-buffer scanning, no external dependency, fatal on
 * error).
 *
 * Implemented: key = value pairs (bare/quoted/dotted keys), [table] and
 * [[array of tables]] headers (with correct "navigate into the *last*
 * element of an existing array-of-table" stepping), inline tables
 * `{ a = 1, b = 2 }`, arrays `[ 1, 2, 3 ]` (free line/comment placement),
 * basic strings with \b \t \n \f \r \" \\ \uXXXX \UXXXXXXXX escapes,
 * literal strings ('...', no escapes), multi-line basic/literal strings
 * (triple-quoted, with the opening-newline-trim and line-ending-backslash
 * splicing rules), integers (decimal/0x/0o/0b, underscore separators,
 * leading +/-), floats and inf/nan (see classify_bare_token/
 * parse_float_token below), booleans, and TOML date/time literals --
 * which real Nix's own fromTOML represents as `{ _type = "timestamp";
 * value = "<text>"; }`, which is just a string wrapped in an attrset and
 * so is fully representable in asnix's tower either way.
 */
#include "toml.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ll/value.h"
#include "builtins_shared.h"
#include "symtab.h"
#include "value_util.h"

static void toml_fatal(const char *msg) {
    fprintf(stderr, "ll: fromTOML: %s\n", msg);
    exit(1);
}


/* ---------------------------------------------------------------------
 * Parser cursor
 * --------------------------------------------------------------------- */

typedef struct {
    const char *s;
    size_t n;
    size_t i;
} Parser;

static int p_at_end(const Parser *p) { return p->i >= p->n; }
static char p_cur(const Parser *p) { return p_at_end(p) ? '\0' : p->s[p->i]; }
static char p_at(const Parser *p, size_t off) { return (p->i + off < p->n) ? p->s[p->i + off] : '\0'; }

static void expect_char(Parser *p, char c) {
    if (p_cur(p) != c) {
        char buf[64];
        snprintf(buf, sizeof(buf), "expected '%c'", c);
        toml_fatal(buf);
    }
    p->i++;
}

/* Spaces/tabs only -- never consumes a newline (TOML's own distinction
 * between intra-line "ws" and the document's line structure). */
static void skip_ws(Parser *p) {
    while (!p_at_end(p) && (p_cur(p) == ' ' || p_cur(p) == '\t')) p->i++;
}

static void skip_to_eol(Parser *p) {
    while (!p_at_end(p) && p_cur(p) != '\n') p->i++;
}

/* Used *between* top-level statements/array elements, where TOML allows
 * arbitrary blank lines and `#`-comments to be interspersed. */
static void skip_ws_comments_newlines(Parser *p) {
    for (;;) {
        if (!p_at_end(p) && (p_cur(p) == ' ' || p_cur(p) == '\t' || p_cur(p) == '\r' || p_cur(p) == '\n')) {
            p->i++;
            continue;
        }
        if (!p_at_end(p) && p_cur(p) == '#') {
            skip_to_eol(p);
            continue;
        }
        break;
    }
}

/* ---------------------------------------------------------------------
 * Mutable document tree (top-level [table]/[[array-of-tables]]/dotted-key
 * structure) -- see toml.h's header comment. Needed because real TOML
 * documents build this incrementally across separate statements (e.g.
 * `a.b = 1` on one line, `a.c = 2` on the next, must merge into one
 * table `a`), unlike inline tables/arrays (which parse straight into an
 * immutable asnix Value the moment their closing bracket is seen, via
 * parse_value's recursion -- see parse_inline_table/parse_array below,
 * which still reuse this same tree type locally for their own dotted-key
 * support but finalize it immediately rather than threading it through
 * the rest of the document).
 * --------------------------------------------------------------------- */

typedef struct TNode TNode;
typedef enum { TN_TABLE, TN_ARRTABLE, TN_LEAF } TNKind;

typedef struct {
    uint32_t sym;
    TNode *child;
} TNEntry;

struct TNode {
    TNKind kind;
    TNEntry *entries;
    size_t n_entries, cap_entries; /* TN_TABLE */
    TNode **items;
    size_t n_items, cap_items; /* TN_ARRTABLE */
    Value leaf;                /* TN_LEAF */
    int declared_as_table;     /* TN_TABLE only: set the moment an
                                 * explicit `[table]` header (not just
                                 * implicit creation while stepping through
                                 * a dotted/table-header path) targets this
                                 * node -- TOML forbids declaring the same
                                 * table twice ("[a]\n[a]" is invalid). */
};

static TNode *tnode_new(TNKind k) {
    TNode *t = calloc(1, sizeof(TNode));
    t->kind = k;
    return t;
}

static TNode *tnode_find_child(TNode *t, uint32_t sym) {
    for (size_t i = 0; i < t->n_entries; i++) {
        if (t->entries[i].sym == sym) return t->entries[i].child;
    }
    return NULL;
}

static void tnode_add_child(TNode *t, uint32_t sym, TNode *child) {
    if (t->n_entries == t->cap_entries) {
        t->cap_entries = t->cap_entries ? t->cap_entries * 2 : 4;
        t->entries = realloc(t->entries, t->cap_entries * sizeof(TNEntry));
    }
    t->entries[t->n_entries].sym = sym;
    t->entries[t->n_entries].child = child;
    t->n_entries++;
}

static void tnode_arrtable_push(TNode *t, TNode *item) {
    if (t->n_items == t->cap_items) {
        t->cap_items = t->cap_items ? t->cap_items * 2 : 4;
        t->items = realloc(t->items, t->cap_items * sizeof(TNode *));
    }
    t->items[t->n_items++] = item;
}

/* Finds (or creates) the TABLE to continue navigating *into* for an
 * intermediate segment of a dotted/table-header key path. Stepping into
 * an existing ARRTABLE's *last* item is what makes `[[a]]` followed by
 * `[[a.b]]` nest `b` under the most-recently-appended `a` element, not
 * under `a` itself (confirmed against lang/eval-okay-fromTOML-046.exp). */
static TNode *tnode_get_or_create_table_child(TNode *t, uint32_t sym) {
    TNode *c = tnode_find_child(t, sym);
    if (!c) {
        c = tnode_new(TN_TABLE);
        tnode_add_child(t, sym, c);
        return c;
    }
    if (c->kind == TN_ARRTABLE) {
        if (c->n_items == 0) toml_fatal("internal error: empty array-of-tables navigated");
        return c->items[c->n_items - 1];
    }
    if (c->kind == TN_TABLE) return c;
    toml_fatal("a key is used both as a value and as a table");
    return NULL;
}

static void tnode_set_leaf(TNode *t, uint32_t sym, Value v) {
    if (tnode_find_child(t, sym)) toml_fatal("duplicate key in TOML document");
    TNode *leaf = tnode_new(TN_LEAF);
    leaf->leaf = v;
    tnode_add_child(t, sym, leaf);
}

static void tnode_insert_leaf_path(TNode *base, const uint32_t *syms, size_t n, Value v) {
    TNode *cur = base;
    for (size_t i = 0; i + 1 < n; i++) cur = tnode_get_or_create_table_child(cur, syms[i]);
    tnode_set_leaf(cur, syms[n - 1], v);
}

static TNode *tnode_enter_table(TNode *root, const uint32_t *syms, size_t n) {
    TNode *cur = root;
    for (size_t i = 0; i + 1 < n; i++) cur = tnode_get_or_create_table_child(cur, syms[i]);
    uint32_t last = syms[n - 1];
    TNode *child = tnode_find_child(cur, last);
    if (!child) {
        child = tnode_new(TN_TABLE);
        tnode_add_child(cur, last, child);
    } else if (child->kind != TN_TABLE) {
        toml_fatal("[table] header redefines a non-table key");
    } else if (child->declared_as_table) {
        /* "[a]\n[a]" -- a table may only be *declared* (via its own
         * [table] header) once; implicitly stepping through it as an
         * ancestor of some other header/dotted-key is fine and doesn't
         * set this flag (see tnode_get_or_create_table_child above). */
        toml_fatal("[table] header redefines an already-declared table");
    }
    child->declared_as_table = 1;
    return child;
}

static TNode *tnode_enter_array_table(TNode *root, const uint32_t *syms, size_t n) {
    TNode *cur = root;
    for (size_t i = 0; i + 1 < n; i++) cur = tnode_get_or_create_table_child(cur, syms[i]);
    uint32_t last = syms[n - 1];
    TNode *child = tnode_find_child(cur, last);
    if (!child) {
        child = tnode_new(TN_ARRTABLE);
        tnode_add_child(cur, last, child);
    } else if (child->kind != TN_ARRTABLE) {
        toml_fatal("[[array of tables]] header redefines a non-array key");
    }
    TNode *item = tnode_new(TN_TABLE);
    tnode_arrtable_push(child, item);
    return item;
}

static Value finalize_tnode(TNode *t) {
    Value out;
    switch (t->kind) {
        case TN_LEAF:
            out = t->leaf;
            break;
        case TN_TABLE: {
            size_t n = t->n_entries;
            uint32_t *syms = malloc((n ? n : 1) * sizeof(uint32_t));
            Value *vals = malloc((n ? n : 1) * sizeof(Value));
            for (size_t i = 0; i < n; i++) {
                syms[i] = t->entries[i].sym;
                vals[i] = finalize_tnode(t->entries[i].child);
            }
            out = ll_bshared_make_attrs(syms, vals, (int64_t)n);
            free(syms);
            free(vals);
            break;
        }
        case TN_ARRTABLE: {
            size_t n = t->n_items;
            Value *vals = malloc((n ? n : 1) * sizeof(Value));
            for (size_t i = 0; i < n; i++) vals[i] = finalize_tnode(t->items[i]);
            out = ll_bshared_make_list(vals, (int64_t)n);
            free(vals);
            break;
        }
        default:
            toml_fatal("internal error: bad TNode kind");
            out = ll_make_imm(LL_IMM_NULL);
            break;
    }
    return out;
}

static void tnode_free(TNode *t) {
    if (!t) return;
    for (size_t i = 0; i < t->n_entries; i++) tnode_free(t->entries[i].child);
    free(t->entries);
    for (size_t i = 0; i < t->n_items; i++) tnode_free(t->items[i]);
    free(t->items);
    free(t);
}

/* ---------------------------------------------------------------------
 * String scanning/decoding
 * --------------------------------------------------------------------- */

static int hex_digit_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    toml_fatal("invalid hex digit in \\u/\\U escape");
    return 0;
}

static uint32_t parse_hex_digits(const char *s, size_t n) {
    uint32_t v = 0;
    for (size_t i = 0; i < n; i++) v = (v << 4) | (uint32_t)hex_digit_value(s[i]);
    return v;
}

/* Standard UTF-8 encoding of a Unicode code point -- returns the byte
 * count written (1..4). No surrogate-pair handling needed: TOML's \U
 * escape is always 8 hex digits of one full code point directly (unlike
 * JSON's UTF-16-surrogate-pair \u\u scheme), and \u is always exactly 4. */
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

/* Finds the end of a single-line quoted run (basic `"..."` or literal
 * `'...'`), returning the index of the closing quote. `escaped` selects
 * whether a backslash protects the following character from ending the
 * scan (basic strings only -- literal strings have no escapes at all, so
 * a bare `\` there is just an ordinary content byte). Does not decode
 * anything -- see decode_basic below for that pass. */
static size_t scan_single_line_string_end(Parser *p, char quote, int escaped) {
    size_t i = p->i;
    while (i < p->n) {
        char c = p->s[i];
        if (escaped && c == '\\') {
            i += 2; /* skip the escaped char verbatim, whatever it is -- see decode_basic */
            continue;
        }
        if (c == quote) return i;
        if (c == '\n') toml_fatal("newline inside a single-line TOML string");
        i++;
    }
    toml_fatal("unterminated TOML string");
    return 0;
}

/* Finds the end of a multi-line quoted run (`"""..."""`/`'''...'''`):
 * the first *unescaped* occurrence of three consecutive quote chars.
 * Shorter runs of 1-2 quote chars (e.g. the `"one"` inside
 * `"""has "one" quote"""`) are ordinary content, not a terminator. */
static size_t scan_multiline_string_end(Parser *p, char quote, int escaped) {
    size_t i = p->i;
    while (i < p->n) {
        char c = p->s[i];
        if (escaped && c == '\\') {
            i += 2;
            continue;
        }
        if (c == quote) {
            if (i + 2 < p->n && p->s[i + 1] == quote && p->s[i + 2] == quote) return i;
            i++;
            continue;
        }
        i++;
    }
    toml_fatal("unterminated multi-line TOML string");
    return 0;
}

/* Decodes a basic string's raw content (escapes processed). When
 * `multiline` is set, a backslash that is the last non-whitespace
 * character on its line ("line-ending backslash") splices away that
 * backslash, the newline, and all further leading whitespace on the next
 * line(s) up to the next non-whitespace character -- TOML's multi-line
 * basic string continuation rule (verified against
 * lang/eval-okay-fromTOML-021.exp: `"""a\<LF>   b"""` decodes to `"ab"`).
 * Output is always <= input length (every escape's source form is at
 * least as long as its decoded form), so `n+1` bytes is always enough. */
static char *decode_basic(const char *s, size_t n, int multiline, size_t *out_len) {
    char *buf = malloc(n + 1);
    size_t o = 0, i = 0;
    while (i < n) {
        char c = s[i];
        if (c != '\\') {
            buf[o++] = c;
            i++;
            continue;
        }
        i++; /* consume '\' */
        if (i >= n) toml_fatal("dangling backslash at end of TOML string");
        if (multiline) {
            size_t j = i;
            while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
            if (j < n && (s[j] == '\n' || (s[j] == '\r' && j + 1 < n && s[j + 1] == '\n'))) {
                /* Line-ending backslash: skip it, the run of spaces/tabs,
                 * the newline, and then ALL further whitespace up to the
                 * next non-whitespace character. */
                i = j;
                while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
                continue;
            }
        }
        char e = s[i++];
        switch (e) {
            case 'b': buf[o++] = '\b'; break;
            case 't': buf[o++] = '\t'; break;
            case 'n': buf[o++] = '\n'; break;
            case 'f': buf[o++] = '\f'; break;
            case 'r': buf[o++] = '\r'; break;
            case '"': buf[o++] = '"'; break;
            case '\\': buf[o++] = '\\'; break;
            case 'u':
                if (i + 4 > n) toml_fatal("truncated \\u escape in TOML string");
                o += utf8_encode(parse_hex_digits(s + i, 4), buf + o);
                i += 4;
                break;
            case 'U':
                if (i + 8 > n) toml_fatal("truncated \\U escape in TOML string");
                o += utf8_encode(parse_hex_digits(s + i, 8), buf + o);
                i += 8;
                break;
            default: toml_fatal("invalid escape sequence in TOML string");
        }
    }
    *out_len = o;
    return buf;
}

/* Parses whichever of the four TOML string forms starts at the parser's
 * current position (basic/literal, single/multi-line), decodes it, and
 * advances the parser past the closing delimiter. Returns a malloc'd
 * buffer (caller frees) -- used both for string *values* (wrapped into a
 * Value by the caller) and for quoted *keys* (interned directly). */
static char *parse_toml_string(Parser *p, size_t *out_len) {
    char quote = p_cur(p);
    int basic = (quote == '"');
    int multiline = (p_at(p, 1) == quote && p_at(p, 2) == quote);
    size_t delim_len = multiline ? 3 : 1;
    p->i += delim_len;

    if (multiline) {
        /* A newline (optionally preceded by \r) immediately after the
         * opening delimiter is trimmed, not part of the content. */
        if (p_cur(p) == '\n') p->i++;
        else if (p_cur(p) == '\r' && p_at(p, 1) == '\n') p->i += 2;
    }

    size_t content_start = p->i;
    size_t content_end = multiline ? scan_multiline_string_end(p, quote, basic) : scan_single_line_string_end(p, quote, basic);
    size_t raw_len = content_end - content_start;
    char *decoded;
    size_t decoded_len;
    if (basic) {
        decoded = decode_basic(p->s + content_start, raw_len, multiline, &decoded_len);
    } else {
        /* Literal strings: zero escape processing, raw bytes verbatim. */
        decoded = malloc(raw_len ? raw_len : 1);
        memcpy(decoded, p->s + content_start, raw_len);
        decoded_len = raw_len;
    }
    p->i = content_end + delim_len;
    /* Real Nix's strings can't carry an embedded NUL byte; a TOML
     * backslash-u0000 escape (or a literal NUL byte straight in the
     * document) decodes to one here with no complaint otherwise, in
     * either a quoted key or a string value -- both paths run through
     * this one function (parse_key_segment calls this for quoted keys
     * too). */
    if (memchr(decoded, '\0', decoded_len)) toml_fatal("string contains a null byte");
    *out_len = decoded_len;
    return decoded;
}

/* ---------------------------------------------------------------------
 * Keys
 * --------------------------------------------------------------------- */

static int bare_key_char(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '-';
}

static char *parse_key_segment(Parser *p, size_t *out_len) {
    if (p_cur(p) == '"' || p_cur(p) == '\'') return parse_toml_string(p, out_len);
    size_t start = p->i;
    while (!p_at_end(p) && bare_key_char(p_cur(p))) p->i++;
    if (p->i == start) toml_fatal("expected a key");
    size_t len = p->i - start;
    char *buf = malloc(len ? len : 1);
    memcpy(buf, p->s + start, len);
    *out_len = len;
    return buf;
}

/* `key ( ws '.' ws key )*` -- caller frees the returned `syms` array. */
static void parse_key_path(Parser *p, uint32_t **syms_out, size_t *n_out) {
    size_t cap = 4, n = 0;
    uint32_t *syms = malloc(cap * sizeof(uint32_t));
    for (;;) {
        size_t len;
        char *buf = parse_key_segment(p, &len);
        if (n == cap) {
            cap *= 2;
            syms = realloc(syms, cap * sizeof(uint32_t));
        }
        syms[n++] = ll_intern(buf, len);
        free(buf);
        skip_ws(p);
        if (p_cur(p) == '.') {
            p->i++;
            skip_ws(p);
            continue;
        }
        break;
    }
    *syms_out = syms;
    *n_out = n;
}

/* ---------------------------------------------------------------------
 * Bare values: booleans, integers, date/time literals -- and detecting
 * (then rejecting) floats/inf/nan, see toml.h's header comment.
 * --------------------------------------------------------------------- */

static int is_digit_run(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
    }
    return n > 0;
}

/* A TOML date/time literal is the only bare-value shape containing `-` at
 * a fixed non-leading position (full-date, `YYYY-MM-DD`) or `:` at all
 * (local-time, `HH:MM:SS`) -- neither can ever appear in an integer
 * (whose only allowed `-` is a leading sign) or a float, so checking
 * these fixed positions unambiguously distinguishes a date/time token
 * from every other bare-value kind before any further parsing. */
static int looks_like_date_or_time(const char *tok, size_t n) {
    if (n >= 10 && is_digit_run(tok, 4) && tok[4] == '-' && is_digit_run(tok + 5, 2) && tok[7] == '-' &&
        is_digit_run(tok + 8, 2))
        return 1;
    if (n >= 8 && is_digit_run(tok, 2) && tok[2] == ':' && is_digit_run(tok + 3, 2) && tok[5] == ':' &&
        is_digit_run(tok + 6, 2))
        return 1;
    return 0;
}

/* Shape *and* value range for a local-time's required `HH:MM:SS` prefix
 * (any `.frac`/offset after the 8th byte is validated/normalized
 * separately by normalize_time -- not this function's concern). Used
 * both for a bare local-time value and for the time half of a
 * date-time, since TOML 1.0.0 requires the *full* HH:MM:SS -- a
 * date-time truncated to just HH:MM (e.g. "1979-05-27T07:32") is
 * invalid, not shorthand. Also rejects out-of-range HH/MM/SS (e.g.
 * "25:00:00"). */
static int valid_time_value(const char *s, size_t n) {
    if (!(n >= 8 && is_digit_run(s, 2) && s[2] == ':' && is_digit_run(s + 3, 2) && s[5] == ':' &&
          is_digit_run(s + 6, 2)))
        return 0;
    int hh = (s[0] - '0') * 10 + (s[1] - '0');
    int mm = (s[3] - '0') * 10 + (s[4] - '0');
    int ss = (s[6] - '0') * 10 + (s[7] - '0');
    return hh <= 23 && mm <= 59 && ss <= 60; /* <=60 allows a leap second */
}

/* Calendar-range check for a `YYYY-MM-DD` token already shape-validated
 * by looks_like_date_or_time's first branch (digit runs and dashes
 * confirmed in place). */
static int valid_date_value(const char *s) {
    int mo = (s[5] - '0') * 10 + (s[6] - '0');
    int dd = (s[8] - '0') * 10 + (s[9] - '0');
    return mo >= 1 && mo <= 12 && dd >= 1 && dd <= 31;
}

/* Fractional seconds are normalized the way real Nix's toml11-backed
 * fromTOML does: padded with trailing zeros up to a minimum of 3 digits
 * (millisecond precision), kept as-is between 3 and 9 digits, truncated
 * beyond 9 (nanosecond precision is TOML's own ceiling). */
static size_t write_normalized_frac(const char *digits, size_t n, char *out) {
    size_t take = n > 9 ? 9 : n;
    memcpy(out, digits, take);
    size_t len = take;
    while (len < 3) out[len++] = '0';
    return len;
}

/* Normalizes `HH:MM:SS[.frac][offset]` (offset = `Z`/`z` or `+HH:MM`/
 * `-HH:MM`) starting at `s`, writing the canonical form (uppercase `Z`,
 * fractional-second padding/truncation per write_normalized_frac, offset
 * otherwise verbatim) into `out`. Returns the number of bytes written. */
static size_t normalize_time(const char *s, size_t n, char *out) {
    memcpy(out, s, 8);
    size_t o = 8, idx = 8;
    if (idx < n && s[idx] == '.') {
        idx++;
        size_t start = idx;
        while (idx < n && s[idx] >= '0' && s[idx] <= '9') idx++;
        size_t flen = idx - start;
        if (flen > 0) {
            out[o++] = '.';
            o += write_normalized_frac(s + start, flen, out + o);
        }
    }
    if (idx < n) {
        if (s[idx] == 'Z' || s[idx] == 'z') {
            out[o++] = 'Z';
            idx++;
        } else if (s[idx] == '+' || s[idx] == '-') {
            memcpy(out + o, s + idx, n - idx);
            o += n - idx;
            idx = n;
        }
    }
    return o;
}

/* The real Nix currently installed in this environment rejects EVERY
 * TOML date/time value (local date, local time, local date-time, offset
 * date-time) with `error: while parsing TOML: Dates and times are not
 * supported`, unconditionally -- so this parser matches that and rejects
 * them too (same `toml_fatal` convention `looks_like_date_or_time`'s
 * sibling TOML-float rejection uses just below), rather than actually
 * producing the `{ _type = "timestamp"; value = "..."; }` attrset some
 * older real-Nix versions produced. `make_timestamp_value`'s
 * attrset-building code is kept dead-but-documented below rather than
 * deleted, in case a future change needs to special-case a SPECIFIC
 * date shape once proven that some real Nix version still accepts it. */
static Value make_timestamp_value(const char *text, size_t len) {
    uint32_t sym_type = ll_intern("_type", 5);
    uint32_t sym_value = ll_intern("value", 5);
    uint32_t syms[2] = { sym_type, sym_value };
    Value vals[2] = { ll_make_string_value("timestamp", 9), ll_make_string_value(text, len) };
    return ll_bshared_make_attrs(syms, vals, 2);
}

static Value classify_date_or_time(const char *tok, size_t n) {
    char buf[96];
    size_t o;
    (void)make_timestamp_value; /* kept for a future milestone -- see comment above */
    if (n >= 10 && tok[4] == '-' && tok[7] == '-') {
        if (!valid_date_value(tok)) toml_fatal("invalid calendar date in TOML document");
        memcpy(buf, tok, 10);
        o = 10;
        if (n > 10) {
            char sep = tok[10];
            if (sep != 'T' && sep != 't' && sep != ' ') toml_fatal("malformed TOML date-time separator");
            if (!valid_time_value(tok + 11, n - 11))
                toml_fatal("invalid or incomplete time-of-day in TOML date-time (requires HH:MM:SS)");
            buf[o++] = 'T';
            o += normalize_time(tok + 11, n - 11, buf + o);
        }
    } else {
        if (!valid_time_value(tok, n)) toml_fatal("invalid or incomplete TOML local-time value (requires HH:MM:SS)");
        o = normalize_time(tok, n, buf);
    }
    (void)o;
    toml_fatal("Dates and times are not supported");
    return ll_make_imm(LL_IMM_NULL); /* unreachable -- toml_fatal always exit()s */
}

/* `_` digit separators must be single and strictly between two digits
 * -- never leading, trailing, or doubled. Checked on the raw `digits`/
 * `n` (before underscores are stripped out below), since that's the
 * only point the original separator positions are still visible. */
static void validate_digit_separators(const char *digits, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (digits[i] != '_') continue;
        if (i == 0 || i + 1 >= n || digits[i - 1] == '_' || digits[i + 1] == '_')
            toml_fatal("invalid digit separator placement in TOML integer literal");
    }
}

static Value parse_decimal_or_prefixed_int(const char *digits, size_t n, int base, int neg) {
    validate_digit_separators(digits, n);
    char buf[160];
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (digits[i] == '_') continue;
        if (o >= sizeof(buf) - 1) toml_fatal("integer literal too long");
        buf[o++] = digits[i];
    }
    buf[o] = '\0';
    if (o == 0) toml_fatal("invalid integer literal");
    /* Decimal integers may not have a leading zero (except "0" itself).
     * Hex/octal/binary literals are exempt (TOML permits e.g. "0x01"):
     * only `base == 10` is checked. */
    if (base == 10 && o > 1 && buf[0] == '0') toml_fatal("leading zero in decimal TOML integer literal");
    char *endptr;
    long long val = strtoll(buf, &endptr, base);
    /* A bare token that isn't true/false, a date/time, or a
     * properly-formed number (e.g. "tru", a truncated "true") would
     * otherwise fall all the way through to here, where strtoll simply
     * converts zero digits and silently returns 0. `endptr == buf`
     * means nothing was consumed at all. */
    if (endptr == buf) toml_fatal("invalid TOML value");
    if (neg) val = -val;
    return ll_make_int((int64_t)val);
}

/* A TOML float, `inf`, or `nan` bare token (`rem`/`rem_n` already has
 * any leading sign stripped into `neg` by the caller, classify_bare_token
 * below). Underscores are stripped the same way parse_decimal_or_prefixed_int
 * already does for integers, then the whole (sign-prefixed) buffer is
 * handed to strtod -- NOT applying `neg` by negating the parsed double
 * afterwards, since glibc's strtod already gets the sign right for "nan"
 * vs "-nan", and real Nix's own fromTOML preserves exactly that written
 * sign on a NaN, unlike a NaN from arithmetic. */
static Value parse_float_token(const char *rem, size_t rem_n, int neg) {
    char buf[160];
    size_t o = 0;
    if (neg) buf[o++] = '-';
    for (size_t i = 0; i < rem_n; i++) {
        if (rem[i] == '_') continue;
        if (o >= sizeof(buf) - 1) toml_fatal("float literal too long");
        buf[o++] = rem[i];
    }
    buf[o] = '\0';
    return ll_make_float_value(strtod(buf, NULL));
}

static Value classify_bare_token(const char *tok, size_t n) {
    if (n == 4 && memcmp(tok, "true", 4) == 0) return ll_make_imm(LL_IMM_TRUE);
    if (n == 5 && memcmp(tok, "false", 5) == 0) return ll_make_imm(LL_IMM_FALSE);

    if (looks_like_date_or_time(tok, n)) return classify_date_or_time(tok, n);

    size_t idx = 0;
    int neg = 0;
    if (n > 0 && (tok[0] == '+' || tok[0] == '-')) {
        neg = (tok[0] == '-');
        idx = 1;
    }
    const char *rem = tok + idx;
    size_t rem_n = n - idx;
    if (rem_n == 0) toml_fatal("invalid TOML value");

    /* `:` never belongs in a number -- it's only valid as part of a
     * full `HH:MM:SS` local-time, which looks_like_date_or_time already
     * would have caught above. Reaching here with a `:` still in the
     * token means a malformed/incomplete time value (e.g. "07:32", a
     * bare hour:minute missing seconds) -- without this check it would
     * fall through into parse_decimal_or_prefixed_int, whose strtoll
     * just stops at the first `:` and silently returns the truncated
     * leading digits (e.g. "07:32" -> the int 7). */
    for (size_t i = 0; i < rem_n; i++) {
        if (rem[i] == ':') toml_fatal("invalid or incomplete TOML local-time value (requires HH:MM:SS)");
    }

    if ((rem_n == 3 && memcmp(rem, "inf", 3) == 0) || (rem_n == 3 && memcmp(rem, "nan", 3) == 0)) {
        return parse_float_token(rem, rem_n, neg);
    }

    if (rem_n > 2 && rem[0] == '0' && (rem[1] == 'x' || rem[1] == 'X')) return parse_decimal_or_prefixed_int(rem + 2, rem_n - 2, 16, neg);
    if (rem_n > 2 && rem[0] == '0' && rem[1] == 'o') return parse_decimal_or_prefixed_int(rem + 2, rem_n - 2, 8, neg);
    if (rem_n > 2 && rem[0] == '0' && rem[1] == 'b') return parse_decimal_or_prefixed_int(rem + 2, rem_n - 2, 2, neg);

    for (size_t i = 0; i < rem_n; i++) {
        if (rem[i] == '.' || rem[i] == 'e' || rem[i] == 'E') return parse_float_token(rem, rem_n, neg);
    }
    return parse_decimal_or_prefixed_int(rem, rem_n, 10, neg);
}

static int bare_value_char(char c) {
    return isalnum((unsigned char)c) || c == '+' || c == '-' || c == '.' || c == '_' || c == ':';
}

/* ---------------------------------------------------------------------
 * Values (strings/arrays/inline-tables/bare tokens) and the top-level
 * document loop.
 * --------------------------------------------------------------------- */

static Value parse_value(Parser *p);

static Value parse_array(Parser *p) {
    expect_char(p, '[');
    size_t cap = 4, n = 0;
    Value *items = malloc(cap * sizeof(Value));
    skip_ws_comments_newlines(p);
    if (p_cur(p) == ']') {
        p->i++;
        free(items);
        return ll_bshared_make_list(NULL, 0);
    }
    for (;;) {
        Value v = parse_value(p);
        if (n == cap) {
            cap *= 2;
            items = realloc(items, cap * sizeof(Value));
        }
        items[n++] = v;
        skip_ws_comments_newlines(p);
        if (p_cur(p) == ',') {
            p->i++;
            skip_ws_comments_newlines(p);
            if (p_cur(p) == ']') {
                p->i++;
                break;
            }
            continue;
        }
        if (p_cur(p) == ']') {
            p->i++;
            break;
        }
        toml_fatal("expected ',' or ']' in TOML array");
    }
    Value out = ll_bshared_make_list(items, (int64_t)n);
    free(items);
    return out;
}

static Value parse_inline_table(Parser *p) {
    expect_char(p, '{');
    TNode *tbl = tnode_new(TN_TABLE);
    skip_ws(p);
    if (p_cur(p) == '}') {
        p->i++;
        Value out = finalize_tnode(tbl);
        tnode_free(tbl);
        return out;
    }
    for (;;) {
        skip_ws(p);
        uint32_t *syms;
        size_t nsyms;
        parse_key_path(p, &syms, &nsyms);
        skip_ws(p);
        expect_char(p, '=');
        skip_ws(p);
        Value v = parse_value(p);
        tnode_insert_leaf_path(tbl, syms, nsyms, v);
        free(syms);
        skip_ws(p);
        if (p_cur(p) == ',') {
            p->i++;
            continue;
        }
        if (p_cur(p) == '}') {
            p->i++;
            break;
        }
        toml_fatal("expected ',' or '}' in inline table");
    }
    Value out = finalize_tnode(tbl);
    tnode_free(tbl);
    return out;
}

static Value parse_value(Parser *p) {
    skip_ws(p);
    if (p_at_end(p)) toml_fatal("expected a value");
    char c = p_cur(p);
    if (c == '"' || c == '\'') {
        size_t len;
        char *buf = parse_toml_string(p, &len);
        Value v = ll_make_string_value(buf, len);
        free(buf);
        return v;
    }
    if (c == '[') return parse_array(p);
    if (c == '{') return parse_inline_table(p);

    size_t start = p->i;
    while (!p_at_end(p) && bare_value_char(p_cur(p))) p->i++;
    size_t len = p->i - start;
    if (len == 0) toml_fatal("expected a value");

    /* TOML permits a single space (not just 'T'/'t') between the date and
     * time halves of a date-time literal (RFC 3339's alternate form) --
     * but a bare space would otherwise end the token, so after an
     * exact 10-char full-date run, peek past one space for a
     * `DD:DD:...`-shaped continuation and splice the two scans together
     * (normalizing the separator to 'T') before classifying. */
    if (len == 10 && p_cur(p) == ' ' && isdigit((unsigned char)p_at(p, 1)) && isdigit((unsigned char)p_at(p, 2)) &&
        p_at(p, 3) == ':') {
        char combined[96];
        memcpy(combined, p->s + start, 10);
        combined[10] = 'T';
        p->i++; /* skip the space */
        size_t time_start = p->i;
        while (!p_at_end(p) && bare_value_char(p_cur(p))) p->i++;
        size_t time_len = p->i - time_start;
        memcpy(combined + 11, p->s + time_start, time_len);
        return classify_bare_token(combined, 11 + time_len);
    }

    return classify_bare_token(p->s + start, len);
}

Value ll_toml_parse(const char *text, size_t len) {
    Parser p = { text, len, 0 };
    TNode *root = tnode_new(TN_TABLE);
    TNode *cur_table = root;

    skip_ws_comments_newlines(&p);
    while (!p_at_end(&p)) {
        if (p_cur(&p) == '[') {
            int is_array = (p_at(&p, 1) == '[');
            p.i += is_array ? 2 : 1;
            skip_ws(&p);
            uint32_t *syms;
            size_t nsyms;
            parse_key_path(&p, &syms, &nsyms);
            skip_ws(&p);
            if (is_array) {
                expect_char(&p, ']');
                expect_char(&p, ']');
                cur_table = tnode_enter_array_table(root, syms, nsyms);
            } else {
                expect_char(&p, ']');
                cur_table = tnode_enter_table(root, syms, nsyms);
            }
            free(syms);
        } else {
            uint32_t *syms;
            size_t nsyms;
            parse_key_path(&p, &syms, &nsyms);
            skip_ws(&p);
            expect_char(&p, '=');
            Value v = parse_value(&p);
            tnode_insert_leaf_path(cur_table, syms, nsyms, v);
            free(syms);
        }

        /* End of statement: ws, optional comment, then newline or EOF. */
        skip_ws(&p);
        if (!p_at_end(&p) && p_cur(&p) == '#') skip_to_eol(&p);
        if (!p_at_end(&p)) {
            if (p_cur(&p) == '\n') {
                p.i++;
            } else if (p_cur(&p) == '\r' && p_at(&p, 1) == '\n') {
                p.i += 2;
            } else {
                toml_fatal("expected end of line");
            }
        }
        skip_ws_comments_newlines(&p);
    }

    Value out = finalize_tnode(root);
    tnode_free(root);
    return out;
}
