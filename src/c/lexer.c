/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
/* strchrnul (used by lex_normal's line-comment skip below) is a GNU
 * extension, not ISO C -- needs an explicit feature-test macro under
 * -std=c11 (same reason eval_file.c defines _DEFAULT_SOURCE for
 * realpath()). */
#define _DEFAULT_SOURCE
#include "lexer.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    CC_SPACE      = 1 << 0, /* isspace: ' ' '\t' '\n' '\v' '\f' '\r' */
    CC_DIGIT      = 1 << 1, /* isdigit: '0'-'9' */
    CC_ALPHA      = 1 << 2, /* isalpha: 'A'-'Z' 'a'-'z' */
    CC_IDENT_CONT = 1 << 3, /* ident_cont's old body: alnum, '_', '\'', '-' */
    CC_PATH_CHAR  = 1 << 4, /* path_char's old body: alnum, '.', '_', '-', '+' */
    CC_URI_SCHEME = 1 << 5, /* uri_scheme_char's old body: alnum, '+', '-', '.' */
    CC_URI_TAIL   = 1 << 6, /* uri_tail_char's old body: alnum, or one of
                              * % / ? : @ & = + $ , _ . ! ~ * ' - */
};

static const unsigned char CHAR_CLASS[256] = {
    ['\t'] = CC_SPACE, ['\n'] = CC_SPACE, ['\v'] = CC_SPACE, ['\f'] = CC_SPACE, ['\r'] = CC_SPACE, [' '] = CC_SPACE,

    ['0' ... '9'] = CC_DIGIT | CC_IDENT_CONT | CC_PATH_CHAR | CC_URI_SCHEME | CC_URI_TAIL,
    ['A' ... 'Z'] = CC_ALPHA | CC_IDENT_CONT | CC_PATH_CHAR | CC_URI_SCHEME | CC_URI_TAIL,
    ['a' ... 'z'] = CC_ALPHA | CC_IDENT_CONT | CC_PATH_CHAR | CC_URI_SCHEME | CC_URI_TAIL,

    ['\''] = CC_IDENT_CONT | CC_URI_TAIL,
    ['_']  = CC_IDENT_CONT | CC_PATH_CHAR | CC_URI_TAIL,
    ['-']  = CC_IDENT_CONT | CC_PATH_CHAR | CC_URI_SCHEME | CC_URI_TAIL,
    ['.']  = CC_PATH_CHAR | CC_URI_SCHEME | CC_URI_TAIL,
    ['+']  = CC_PATH_CHAR | CC_URI_SCHEME | CC_URI_TAIL,

    ['!'] = CC_URI_TAIL, ['$'] = CC_URI_TAIL, ['%'] = CC_URI_TAIL, ['&'] = CC_URI_TAIL,
    ['*'] = CC_URI_TAIL, [','] = CC_URI_TAIL, ['/'] = CC_URI_TAIL, [':'] = CC_URI_TAIL,
    ['='] = CC_URI_TAIL, ['?'] = CC_URI_TAIL, ['@'] = CC_URI_TAIL, ['~'] = CC_URI_TAIL,
};

static int cc_digit(char c) { return (CHAR_CLASS[(unsigned char)c] & CC_DIGIT) != 0; }
static int cc_space(char c) { return (CHAR_CLASS[(unsigned char)c] & CC_SPACE) != 0; }
static int cc_alpha(char c) { return (CHAR_CLASS[(unsigned char)c] & CC_ALPHA) != 0; }

static Token mk_tok(TokKind k, size_t start, size_t end) {
    return (Token){ .kind = k, .start = (uint32_t)start, .end = (uint32_t)end };
}

/* Path-literal scanning (import-paths). Real Nix's lexer recognizes PATH
 * tokens with the (flex) regex `[A-Za-z0-9._+-]*(\/[A-Za-z0-9._+-]+)+\/?`
 * -- an optional prefix of "path chars", followed by one-or-more `/segment`
 * groups, with an optional trailing slash (rejected later by the parser,
 * not the lexer: "path has a trailing slash"). The `+` group is what
 * makes this unambiguous against every other token in this grammar:
 * nothing else in the language ever uses `/` (there's no division
 * operator), so any occurrence of `/` that isn't inside a string is a
 * path. That's also why this is tried *before* digit/ident scanning
 * below, not after: `foo/bar.nix` must lex as one TOK_PATH, not
 * TOK_IDENT("foo") followed by an unrecognized '/'. */
static int path_char(char c) {
    return (CHAR_CLASS[(unsigned char)c] & CC_PATH_CHAR) != 0;
}

static int at_dollar_brace(const char *src, size_t j) {
    return src[j] == '$' && src[j + 1] == '{';
}

/* Scans zero or more "/PATH_CHAR+" groups starting at *j (which must
 * already be positioned past any PATH_CHAR* prefix), mirroring real Nix's
 * own PATH/HPATH lexer.l grammar's group-loop, generalized with ONE
 * addition: a '/' immediately followed by "${" ends the scan right there
 * (consuming that slash, even though zero path_chars followed it)
 * instead of just stopping (needed for `./${foo}`/`~/${foo}`).
 * Deliberately does NOT special-case a trailing bare '/' with nothing
 * useful after it -- that's left for the caller to optionally consume as
 * the grammar's own separate trailing `/?`, gated on `*saw_group` already
 * being true: if this function consumed an ungrouped trailing '/'
 * unconditionally, "a // b" would mis-lex its first '/' as a 1-char path
 * and its second '/' as another, silently breaking TOK_SLASHSLASH
 * detection. Returns the new position. */
static size_t scan_path_groups(const char *src, size_t j, int *saw_group) {
    for (;;) {
        if (src[j] != '/') break;
        size_t seg_start = j + 1;
        size_t k = seg_start;
        while (path_char(src[k])) k++;
        if (k == seg_start) {
            if (at_dollar_brace(src, k)) {
                j = k;
                *saw_group = 1;
            }
            break;
        }
        j = k;
        *saw_group = 1;
    }
    return j;
}

/* Returns the length of a path literal starting at src[i], or 0 if this
 * position isn't one (in which case the caller falls through to ordinary
 * digit/ident/single-char scanning). Also succeeds on a prefix that ends
 * right before "${" (e.g. "./" in "./${foo}", or "/" in "/${foo}"), even
 * though that prefix alone wouldn't otherwise satisfy the "at least one
 * full /PATH_CHAR+ group" rule -- see scan_path_groups. The caller
 * (ll_lex's main loop) is responsible for noticing that case (checking
 * at_dollar_brace right after this returns) and switching into MODE_PATH
 * instead of emitting one complete, standalone TOK_PATH. */
static size_t try_scan_path(const char *src, size_t i) {
    size_t j = i;
    while (path_char(src[j])) j++;
    int saw_group = 0;
    j = scan_path_groups(src, j, &saw_group);
    if (!saw_group) return 0;
    if (src[j] == '/') j++; /* optional trailing slash -- rejected later, in the compiler,
                              * unless interpolation continues right after it */
    return j - i;
}

/* `~`-prefixed ("home") path literal: unlike a plain path, there's no
 * PATH_CHAR* prefix allowed before the first '/' at all -- real Nix's
 * HPATH grammar is exactly `~(/PATH_CHAR+)+/?`, so this only looks for a
 * literal "~/" at src[i] and then reuses scan_path_groups starting right
 * at that '/'. Returns 0 if src[i] isn't the start of a valid HPATH, in
 * which case the caller leaves `i` untouched and falls through to
 * ordinary single-char scanning. */
static size_t try_scan_home_path(const char *src, size_t i) {
    if (src[i] != '~' || src[i + 1] != '/') return 0;
    int saw_group = 0;
    size_t j = scan_path_groups(src, i + 1, &saw_group);
    if (!saw_group) return 0;
    if (src[j] == '/') j++; /* optional trailing slash, same rule as try_scan_path */
    return j - i;
}

/* Bare URL literals (deprecated-but-default-accepted real-Nix syntax).
 * Ported directly from real Nix's own lexer.l `URI` regex:
 *   [a-zA-Z][a-zA-Z0-9\+\-\.]*\:[a-zA-Z0-9\%\/\?\:\@\&\=\+\$\,\_\.\!\~\*\'\-]+
 * Tried in the main loop BEFORE ordinary identifier/keyword scanning, same
 * "longest match wins" precedence flex itself gives this rule over ID:
 * e.g. `x:x+1` lexes as ONE URI token "x:x+1", not as a lambda `x: x+1` --
 * a well-known real-Nix footgun (confirmed against `nix-instantiate`:
 * `(x:x+1) 5` fails with "attempt to call something which is not a
 * function but a string"). Scanning URIs before identifiers is required
 * for this precedence to hold. */
static int uri_scheme_char(char c) {
    return (CHAR_CLASS[(unsigned char)c] & CC_URI_SCHEME) != 0;
}
static int uri_tail_char(char c) {
    return (CHAR_CLASS[(unsigned char)c] & CC_URI_TAIL) != 0;
}
static size_t try_scan_uri(const char *src, size_t i) {
    if (!cc_alpha(src[i])) return 0;
    size_t j = i + 1;
    while (uri_scheme_char(src[j])) j++;
    if (src[j] != ':') return 0;
    size_t k = j + 1;
    size_t tail_start = k;
    while (uri_tail_char(src[k])) k++;
    if (k == tail_start) return 0; /* need >=1 tail char after the ':' */
    return k - i;
}

static int ident_start(char c) {
    return cc_alpha(c) || c == '_';
}
static int ident_cont(char c) {
    /* Real Nix's own identifier regex is `[a-zA-Z_][a-zA-Z0-9_'-]*`, i.e.
     * a hyphen is an ordinary identifier character, not just attrpath-
     * binding syntax: whitespace-free `a-b` is one identifier lookup, NOT
     * `a - b` subtraction -- subtraction only "wins" when at least one
     * side has separating whitespace, by ordinary maximal-munch
     * tokenization (same principle as C's `a+++b`). */
    return (CHAR_CLASS[(unsigned char)c] & CC_IDENT_CONT) != 0;
}

typedef struct {
    const char *kw;
    uint8_t kwlen;
    TokKind kind;
} Keyword;

#define KW(lit, k) { lit, sizeof(lit) - 1, k }

static const Keyword KEYWORDS[] = {
    KW("let", TOK_LET),         KW("in", TOK_IN),     KW("if", TOK_IF),
    KW("then", TOK_THEN),       KW("else", TOK_ELSE),  KW("true", TOK_TRUE),
    KW("false", TOK_FALSE),     KW("null", TOK_NULL),
    KW("rec", TOK_REC),         KW("inherit", TOK_INHERIT),
    KW("assert", TOK_ASSERT),   KW("with", TOK_WITH),
    KW("or", TOK_OR),
};

/* Exhaustively mirrors `KEYWORDS` above (same literals, same TokKind
 * mapping) via a hand-written length-first dispatch instead of a linear
 * scan of the table. Any future edit to KEYWORDS must be mirrored here --
 * an un-mirrored new keyword lexes as TOK_IDENT, a mismatched TokKind
 * mis-parses. */
static TokKind keyword_lookup(const char *s, size_t len) {
    switch (len) {
    case 2:
        if (memcmp(s, "in", 2) == 0) return TOK_IN;
        if (memcmp(s, "if", 2) == 0) return TOK_IF;
        if (memcmp(s, "or", 2) == 0) return TOK_OR;
        return TOK_IDENT;
    case 3:
        if (memcmp(s, "let", 3) == 0) return TOK_LET;
        if (memcmp(s, "rec", 3) == 0) return TOK_REC;
        return TOK_IDENT;
    case 4:
        if (memcmp(s, "then", 4) == 0) return TOK_THEN;
        if (memcmp(s, "else", 4) == 0) return TOK_ELSE;
        if (memcmp(s, "true", 4) == 0) return TOK_TRUE;
        if (memcmp(s, "null", 4) == 0) return TOK_NULL;
        if (memcmp(s, "with", 4) == 0) return TOK_WITH;
        return TOK_IDENT;
    case 5:
        if (memcmp(s, "false", 5) == 0) return TOK_FALSE;
        return TOK_IDENT;
    case 6:
        if (memcmp(s, "assert", 6) == 0) return TOK_ASSERT;
        return TOK_IDENT;
    case 7:
        if (memcmp(s, "inherit", 7) == 0) return TOK_INHERIT;
        return TOK_IDENT;
    default:
        return TOK_IDENT;
    }
}

/* Runs once per process: verifies `keyword_lookup` agrees with `KEYWORDS`
 * on every entry, so the hand-mirrored switch above can never silently
 * drift from the canonical table. */
static void keyword_lookup_selfcheck(void) {
    static int checked = 0;
    if (checked) return;
    checked = 1;
    for (size_t k = 0; k < sizeof(KEYWORDS) / sizeof(KEYWORDS[0]); k++) {
        TokKind got = keyword_lookup(KEYWORDS[k].kw, KEYWORDS[k].kwlen);
        assert(got == KEYWORDS[k].kind);
    }
}

#undef KW

/* Lexer mode stack -- a from-scratch equivalent of real Nix's flex state
 * stack (see ~/nix's src/libexpr/lexer.l, %option stack / PUSH_STATE /
 * POP_STATE): entering a string pushes STRING/ISTRING on top; every "${"
 * (and every literal "{", for exactly the same reason: `${ { a = 1; }.a }`
 * needs the inner attrset's own "}" to NOT prematurely resume string
 * lexing) pushes NORMAL; every "}" pops one level. Only when a pop reveals
 * STRING/ISTRING underneath does the "}" that closed an interpolation
 * actually mean "resume string-content lexing". */
typedef enum { MODE_NORMAL, MODE_STRING, MODE_ISTRING, MODE_PATH } LexMode;

struct Lexer {
    const char *src;
    size_t pos;
    LexMode *modes;
    size_t modes_len, modes_cap;
    /* Mirror of modes[modes_len-1], kept in sync by mode_push/mode_pop.
     * Invariant: EVERY mutation of modes/modes_len must happen through
     * those two functions (plus mode_top's read) -- nothing else in this
     * file ever touches a Lexer's mode fields directly, or `cur` desyncs
     * from the real top of the stack. */
    LexMode cur;
    Token pending;
    int has_pending;
    /* Marker-reparse addition (parser.c's ll_parse_subexpr): 0 for
     * every ordinary Lexer (lexer_new), whose mode stack starts empty
     * and only ever grows from THIS SAME Lexer's own `{`/`${` tokens --
     * for that case, reaching a `}` with nothing left to pop (modes_len
     * == 1) really is a genuine syntax error (an actual unmatched '}' in
     * the source). Set to 1 only by lexer_new_lenient, used when lexing
     * starts PARTWAY into a larger buffer whose own enclosing `{`/`${`
     * (if any) this Lexer never saw opened: ll_parse_subexpr's
     * caller only ever wants ONE expression's worth of real tokens, but
     * the 2-token lookahead this file's own cur()/peek2() convention
     * needs (see lexer_new's own comment) unavoidably fetches one token
     * PAST that expression's own end before parse_expr ever returns --
     * if that lookahead token happens to be the `}` that closes some
     * OUTER construct (an enclosing attrset/rec/string-interpolation
     * this Lexer has no record of), it is NOT a real syntax error, just
     * an artifact of always needing one token of lookahead; the fix is
     * to not crash on it, since ll_parse_subexpr's caller (compiler.c)
     * immediately discards the whole Lexer right after parse_expr
     * returns and never actually consumes this one over-read token
     * either way. */
    int lenient;
};

static void mode_push(Lexer *lx, LexMode mode) {
    if (lx->modes_len == lx->modes_cap) {
        lx->modes_cap = lx->modes_cap ? lx->modes_cap * 2 : 16;
        lx->modes = realloc(lx->modes, lx->modes_cap * sizeof(LexMode));
    }
    lx->modes[lx->modes_len++] = mode;
    lx->cur = mode;
}
static LexMode mode_top(Lexer *lx) {
    return lx->cur;
}
static void mode_pop(Lexer *lx) {
    /* Every push above is paired 1:1 with a pop at a matching "}"/closing
     * quote, driven entirely by well-formed-by-construction scan logic
     * below -- an underflow here would be a lexer bug, not malformed
     * input (malformed `}`-without-`{` is caught before this is ever
     * called, see the MODE_NORMAL '}' case). */
    lx->modes_len--;
    lx->cur = lx->modes[lx->modes_len - 1];
}

static void fatal_unterminated(const char *what) {
    fprintf(stderr, "ll: unterminated %s\n", what);
    exit(1);
}

/* Decodes one backslash-escape's trailing character, matching real Nix's
 * unescapeStr (~/nix's src/libexpr/lexer.l): \n \r \t map to the control
 * character, anything else (including \" \\ \$ \') passes through as the
 * literal character itself. */
static char decode_escape_char(char c) {
    switch (c) {
        case 'n': return '\n';
        case 'r': return '\r';
        case 't': return '\t';
        default: return c;
    }
}

double ll_decode_float_literal(const char *src, uint32_t start, uint32_t end) {
    (void)end;
    errno = 0;
    double d = strtod(src + start, NULL);
    if (errno == ERANGE) {
        fprintf(stderr, "ll: invalid float '%.*s'\n", (int)(end - start), src + start);
        exit(1);
    }
    return d;
}

char *ll_decode_dquote_span(const char *src, uint32_t start, uint32_t end, size_t *out_len) {
    size_t cap = (size_t)(end - start);
    char *out = malloc(cap ? cap : 1);
    size_t n = 0;
    uint32_t i = start;
    while (i < end) {
        char c = src[i];
        if (c == '\\') {
            out[n++] = decode_escape_char(src[i + 1]);
            i += 2;
        } else {
            out[n++] = c;
            i += 1;
        }
    }
    *out_len = n;
    return out;
}

void ll_decode_istr_esc(const char *src, uint32_t start, char out[2], size_t *out_len) {
    char third = src[start + 2];
    if (third == '\'') {
        out[0] = '\''; out[1] = '\'';
        *out_len = 2;
    } else if (third == '$') {
        out[0] = '$';
        *out_len = 1;
    } else { /* '\\' */
        out[0] = decode_escape_char(src[start + 3]);
        *out_len = 1;
    }
}

/* Scans one run of double-quoted string content starting at *i (which
 * must be positioned just inside the opening '"' or just after a closed
 * "${ ... }" interpolation), up to and including the next boundary --
 * either the closing '"' or the next "${" -- and pushes exactly two
 * tokens: a TOK_STR_PART (possibly zero-length, e.g. between two adjacent
 * interpolations) followed by the boundary token (TOK_STR_END or
 * TOK_DOLLAR_LBRACE). Mutates *i and `modes` to reflect the boundary
 * crossed (pop back to NORMAL on close, push a fresh NORMAL on "${").
 * The TOK_STR_PART pushed here is a RAW span -- escapes are scanned over
 * (to find where the span ends and to validate there's no unterminated
 * trailing backslash) but never decoded; decoding happens later, on
 * demand, via ll_decode_dquote_span.
 *
 * Produces exactly ONE Token per call -- a content chunk still returns
 * first; whenever this step's own boundary token (TOK_STR_END or
 * TOK_DOLLAR_LBRACE) is ready at the SAME time (the mode-stack change has
 * to happen atomically with it), that second token is stashed in
 * `lx->pending` for the very next call to hand back immediately. `lx->pos`
 * always ends up at the position reflecting BOTH tokens already consumed,
 * even though only the first is returned here -- the pending slot alone
 * is what makes the second one still show up, in order, on the next
 * call. */
static Token lex_dquote_run(Lexer *lx) {
    const char *src = lx->src;
    size_t start = lx->pos;
    size_t i = lx->pos;
    for (;;) {
        char c = src[i];
        if (c == '\0') fatal_unterminated("string literal");
        if (c == '"') {
            Token part = mk_tok(TOK_STR_PART, start, i);
            i++;
            mode_pop(lx);
            lx->pos = i;
            lx->pending = mk_tok(TOK_STR_END, i, i);
            lx->has_pending = 1;
            return part;
        }
        /* "$$" is always a literal, atomically-consumed pair -- confirmed
         * against real nix-instantiate: "a$${foo}b" passes through
         * unchanged (no lookup of `foo` at all), while an ODD run of
         * dollars ("a$$${foo}b") leaves exactly one unpaired "$" that DOES
         * start real interpolation. Must be checked before the
         * "${"-starts-interpolation rule below, and must `continue` rather
         * than fall into it -- otherwise the second "$" of a literal pair
         * gets misread as the start of a fresh "${" check. Do NOT remove
         * this check: without it, real nixpkgs code using this idiom
         * (accounts-qt's `$${INSTALL_PREFIX}`, a literal qmake variable
         * reference, not a Nix one) silently misparses as an undefined-
         * variable interpolation. */
        if (c == '$' && src[i + 1] == '$') {
            i += 2;
            continue;
        }
        if (c == '$' && src[i + 1] == '{') {
            Token part = mk_tok(TOK_STR_PART, start, i);
            i += 2;
            mode_push(lx, MODE_NORMAL);
            lx->pos = i;
            lx->pending = mk_tok(TOK_DOLLAR_LBRACE, i - 2, i);
            lx->has_pending = 1;
            return part;
        }
        if (c == '\\') {
            char e = src[i + 1];
            if (e == '\0') fatal_unterminated("string literal");
            i += 2;
            continue;
        }
        i++; /* includes raw '\n' -- real Nix allows literal newlines in "..." */
    }
}

/* Scans one or more runs of indented ('' ... '') string content starting
 * at *i, up to and including the next boundary (closing "''" or "${"),
 * pushing one TOK_ISTR_PART per plain-content run and one TOK_ISTR_ESC
 * per escape unit (so indentation stripping, done later over the whole
 * collected sequence, can tell real content from an escape the same way
 * real Nix's StringToken::hasIndentation does -- see ~/nix's
 * ParserState::stripIndentation) followed by the boundary token.
 * Implements the escapes verified against real nix-instantiate: ''$ ->
 * literal "$" (combined with plain "{" right after, this is how ''${
 * becomes a literal "${" without starting an interpolation -- there is
 * no separate "''${" rule, it falls out of this one plus ordinary
 * content scanning), ''' -> literal "''", ''\<c> -> a backslash-style
 * escape of <c>, and bare '' (not followed by one of the above) always
 * closes the string. Both TOK_ISTR_PART and TOK_ISTR_ESC carry a RAW span
 * only -- decoding happens on demand, see ll_decode_istr_esc.
 *
 * Produces exactly one Token per call, same pending-slot convention as
 * lex_dquote_run above for the two unconditional-pair boundaries (closing
 * "''" and "${"). The escape case (''$ / ''' / ''\<c>) is handled WITHOUT
 * the pending slot at all, by a different trick: when a nonempty content
 * run precedes the escape, this returns just the TOK_ISTR_PART chunk and
 * -- critically -- leaves `lx->pos` sitting right BEFORE the escape's own
 * bytes (not past them), rather than consuming it here. The very next
 * call re-enters this same function fresh with `content_start ==
 * lx->pos` (recomputed at entry, see below) landing exactly on that same
 * escape; this time `i > content_start` is false (no unflushed content
 * ahead of it), so it falls straight to the `lx->pos = esc_end; return
 * TOK_ISTR_ESC` branch instead of looping back into content-scanning --
 * i.e. the SAME escape is "re-detected", not re-decoded or duplicated, by
 * ordinary re-entry rather than a stashed second token. Safe only because
 * content_start is always freshly derived from lx->pos at this function's
 * own top, never carried across calls any other way. */
static Token lex_indented_run(Lexer *lx) {
    const char *src = lx->src;
    size_t content_start = lx->pos;
    size_t i = lx->pos;

    for (;;) {
        char c = src[i];
        if (c == '\0') fatal_unterminated("indented string literal (''...'')");

        /* See lex_dquote_run's own comment on the identical rule --
         * "$$" is always a literal, atomically-consumed pair, checked
         * before "${"-starts-interpolation so the second "$" of a pair
         * never gets re-read as the start of a fresh "${" check. */
        if (c == '$' && src[i + 1] == '$') {
            i += 2;
            continue;
        }
        if (c == '$' && src[i + 1] == '{') {
            Token part = mk_tok(TOK_ISTR_PART, content_start, i);
            i += 2;
            mode_push(lx, MODE_NORMAL);
            lx->pos = i;
            lx->pending = mk_tok(TOK_DOLLAR_LBRACE, i - 2, i);
            lx->has_pending = 1;
            return part;
        }

        if (c == '\'' && src[i + 1] == '\'') {
            char third = src[i + 2];
            if (third == '\'' || third == '$' || third == '\\') {
                size_t esc_start = i;
                size_t esc_end;
                if (third == '\\') {
                    if (src[i + 3] == '\0') fatal_unterminated("indented string literal (''...'')");
                    esc_end = i + 4;
                } else {
                    esc_end = i + 3; /* ''' or ''$ -- both exactly 3 raw bytes */
                }
                /* Flush the pending content run only if nonempty (a
                 * content-to-escape category change doesn't force an empty
                 * PART the way a real boundary does). Returning here
                 * WITHOUT consuming the escape (lx->pos left at the
                 * escape's own start) is what makes the next call
                 * re-detect and emit it alone -- see this function's own
                 * header comment. */
                if (i > content_start) {
                    lx->pos = i;
                    return mk_tok(TOK_ISTR_PART, content_start, i);
                }
                lx->pos = esc_end;
                return mk_tok(TOK_ISTR_ESC, esc_start, esc_end);
            }
            /* Bare '' not followed by ', $, or \\ -- always the closing delimiter. */
            Token part = mk_tok(TOK_ISTR_PART, content_start, i);
            i += 2;
            mode_pop(lx);
            lx->pos = i;
            lx->pending = mk_tok(TOK_ISTR_END, i, i);
            lx->has_pending = 1;
            return part;
        }

        /* Plain content: everything else, including a lone '$' not
         * followed by '{', a lone '\'' not followed by another '\'', and
         * raw newlines (which indentation-stripping treats specially) --
         * just extend the pending content run, nothing to write yet.
         *
         * Jump ahead to the next CANDIDATE boundary byte ('$' or '\'')
         * via strchrnul instead of advancing one byte at a time (safe:
         * strchrnul never reads past the string's own NUL terminator).
         * Must search from i+1, not i: the byte at i, if it IS '$' or
         * '\'', already failed the real-boundary checks above, so
         * re-finding it at the same position would never advance (and
         * loop forever). A landed-on candidate that turns out to be
         * another false positive just falls through these same checks
         * again at the top of the loop, exactly as a plain byte would. */
        const char *dollar = strchrnul(src + i + 1, '$');
        const char *squote = strchrnul(src + i + 1, '\'');
        i = (size_t)((dollar < squote ? dollar : squote) - src);
    }
}

/* Scans one run of path-literal content starting at *i (positioned just
 * after a closed "${ ... }" interpolation inside a path literal whose
 * FIRST chunk already established it continues past one -- see the
 * MODE_PATH push sites in ll_lex's main loop above), up to the next
 * boundary: either another "${" or the first character that isn't a
 * path continuation at all (ending the whole literal). Pushes exactly
 * one TOK_PATH_PART (the literal chunk just scanned, possibly
 * zero-length) followed by the boundary token (TOK_DOLLAR_LBRACE or
 * TOK_PATH_END). Unlike lex_dquote_run/lex_indented_run, there is no
 * escape processing at all in path-literal syntax (no backslash is ever
 * a path_char), so this chunk's span already points straight at the
 * final text. A chunk may start with plain path_chars, a '/'-group run,
 * or both, exactly like a fresh path literal's own scan (reuses
 * scan_path_groups the same way try_scan_path does), EXCEPT it's never
 * required to contain any group at all (a bare trailing `-b`-style run
 * with no slash, like `./a-${foo}-b`'s "-b", is perfectly valid here --
 * only the very FIRST chunk of a path literal needs that requirement, to
 * tell a path apart from "not a path at all"). */
static Token lex_path_run(Lexer *lx) {
    const char *src = lx->src;
    size_t start = lx->pos;
    size_t j = lx->pos;
    while (path_char(src[j])) j++;
    int saw_group = 0;
    j = scan_path_groups(src, j, &saw_group);
    if (saw_group && src[j] == '/') j++; /* optional trailing slash, same rule as try_scan_path */
    size_t i = j;

    if (at_dollar_brace(src, i)) {
        Token part = mk_tok(TOK_PATH_PART, start, i);
        i += 2;
        mode_push(lx, MODE_NORMAL); /* MODE_PATH underneath is left in place -- the matching
                                      * '}' pops back to it, re-entering this same function */
        lx->pos = i;
        lx->pending = mk_tok(TOK_DOLLAR_LBRACE, i - 2, i);
        lx->has_pending = 1;
        return part;
    }
    Token part = mk_tok(TOK_PATH_PART, start, i);
    mode_pop(lx);
    lx->pos = i;
    lx->pending = mk_tok(TOK_PATH_END, i, i);
    lx->has_pending = 1;
    return part;
}

/* Shared tail-scan for a float literal already known to match (either a
 * digit run followed by '.', or a bare '.' followed by a digit) -- see
 * lex_until's two call sites below. `*i` arrives already positioned just
 * past the decisive '.'; this scans the fractional digits and an
 * optional exponent ONLY (no strtod call here -- that's deferred to
 * ll_decode_float_literal, called later from parser.c once an AST_FLOAT
 * node is actually being built). Mirrors real Nix's own lexer.l FLOAT
 * regex exactly. */
static void scan_float_tail(const char *src, size_t *i) {
    while (cc_digit(src[*i])) (*i)++;
    if (src[*i] == 'e' || src[*i] == 'E') {
        size_t k = *i + 1;
        if (src[k] == '+' || src[k] == '-') k++;
        if (cc_digit(src[k])) {
            *i = k;
            while (cc_digit(src[*i])) (*i)++;
        } /* else: 'e'/'E' not actually followed by a valid exponent -- leave *i unchanged, unconsumed */
    }
}

/* Produces exactly ONE Token per call, by value. The handful of steps
 * that push TWO tokens in one go (a path/URI chunk immediately followed
 * by a "${" interpolation boundary) stash the second one in
 * `lx->pending` for the very next call, exactly like lex_dquote_run/
 * lex_indented_run/lex_path_run above. */
static Token lex_normal(Lexer *lx) {
    const char *src = lx->src;
    for (;;) {
        size_t i = lx->pos;
        char c = src[i];
        size_t tok_start = i; /* byte offset of whatever token (if any) starts here */

        if (c == '\0') {
            if (lx->modes_len != 1) {
                fprintf(stderr, "ll: unexpected end of input inside '${ ... }'\n");
                exit(1);
            }
            lx->pos = i; /* left unchanged: a repeated call at EOF keeps producing TOK_EOF */
            return mk_tok(TOK_EOF, i, i);
        }

        if (cc_space(c)) {
            /* cc_space('\0') is false, so this stops at end of input
             * exactly like the outer loop's own '\0' case would. */
            do { i++; } while (cc_space(src[i]));
            lx->pos = i;
            continue;
        }
        if (c == '#') { /* line comment, a harmless real-Nix-ism to allow in fixtures */
            i = (size_t)(strchrnul(src + i, '\n') - src);
            lx->pos = i;
            continue;
        }
        if (c == '/' && src[i + 1] == '*') { /* block comment. Checked before try_scan_path
                                               * below: comments must never compete with
                                               * path-literal scanning. */
            i += 2;
            for (;;) {
                i = (size_t)(strchrnul(src + i, '*') - src);
                if (!src[i] || src[i + 1] == '/') break;
                i++;
            }
            if (!src[i]) {
                fprintf(stderr, "ll: unterminated block comment\n");
                exit(1);
            }
            i += 2;
            lx->pos = i;
            continue;
        }

        /* Tried before digit/string/single-char below -- see
         * try_scan_path's comment for why this has to come first.
         * `~`-prefixed home paths are tried alongside (src[i]=='~' is
         * never itself a path_char, so try_scan_path can never fire at
         * this position on its own -- a separate probe is required). Both
         * branches below handle the "immediately followed by ${" case
         * identically: emit just this chunk as TOK_PATH, then push
         * MODE_PATH (so the matching '}' that closes the interpolation's
         * expression returns here, not to plain MODE_NORMAL) underneath a
         * fresh MODE_NORMAL for the expression itself, and emit
         * TOK_DOLLAR_LBRACE -- exactly the same push-two-modes-and-emit-
         * the-boundary-token shape lex_dquote_run/lex_indented_run already
         * use for "${" inside a string. */
        {
            size_t hlen = try_scan_home_path(src, i);
            if (hlen > 0) {
                size_t end = i + hlen;
                if (at_dollar_brace(src, end)) {
                    mode_push(lx, MODE_PATH);
                    mode_push(lx, MODE_NORMAL);
                    lx->pos = end + 2;
                    lx->pending = mk_tok(TOK_DOLLAR_LBRACE, end, end + 2);
                    lx->has_pending = 1;
                    return mk_tok(TOK_PATH, i, end);
                }
                lx->pos = end;
                return mk_tok(TOK_PATH, i, end);
            }
        }

        /* Identifier-leading tokens get their own guarded path/URI check
         * instead of unconditionally calling try_scan_path (and, for
         * alpha leads, try_scan_uri): scan the ordinary identifier's own
         * ident_cont run ONCE (needed anyway to produce a plain
         * TOK_IDENT/keyword), then make the expensive path_char-run-plus-
         * group scan and URI scan conditional on a cheap single-byte peek
         * at the character immediately following that run.
         *
         * Correctness: ident_cont and path_char/uri_scheme_char are NOT
         * the same character set. '.' and '+' are path_char/
         * uri_scheme_char but NOT ident_cont -- e.g. a token shaped like
         * "a.b:foo" (a URI with a scheme containing '.') or "foo.bar/baz"
         * (a path literal with '.' in its prefix) needs the real
         * path_char/uri_scheme_char run, which keeps going past where
         * ident_cont already stopped -- not just where the identifier's
         * own run ends. Conversely, '\'' is ident_cont but NOT path_char/
         * uri_scheme_char.
         *
         * The guard below fires the real (expensive) rescan -- from this
         * token's own start, not from the ident_cont run's end -- whenever
         * the byte right after the ident_cont run is '/' (direct
         * path-group signal), ':' (direct URI-colon signal), or '.'/'+'
         * (the ident_cont run stopped short of where path_char/
         * uri_scheme_char would keep going -- ambiguous without the real
         * rescan, exactly the "a.b:foo"/"foo.bar/baz" case above).
         * Whenever NONE of those four hold, skipping both scans entirely
         * is PROVABLY equivalent to running them and having them both
         * fail, not merely a fast-path approximation. Do not weaken this
         * condition without re-checking that proof -- it is what makes
         * skipping the scans safe. */
        if (ident_start(c)) {
            size_t j = i + 1;
            while (ident_cont(src[j])) j++;
            char nb = src[j];
            if (nb == '/' || nb == ':' || nb == '.' || nb == '+') {
                size_t plen = try_scan_path(src, i);
                if (plen > 0) {
                    size_t end = i + plen;
                    if (at_dollar_brace(src, end)) {
                        mode_push(lx, MODE_PATH);
                        mode_push(lx, MODE_NORMAL);
                        lx->pos = end + 2;
                        lx->pending = mk_tok(TOK_DOLLAR_LBRACE, end, end + 2);
                        lx->has_pending = 1;
                        return mk_tok(TOK_PATH, i, end);
                    }
                    lx->pos = end;
                    return mk_tok(TOK_PATH, i, end);
                }
                size_t ulen = try_scan_uri(src, i);
                if (ulen > 0) {
                    lx->pos = i + ulen;
                    return mk_tok(TOK_URI, i, i + ulen);
                }
            }
            /* Plain identifier or keyword: [i, j), with j already computed
             * above by the ident_cont scan this guard needed anyway. */
            size_t start = i;
            size_t len = j - start;
            TokKind kind = keyword_lookup(src + start, len);
            lx->pos = j;
            return mk_tok(kind, start, j);
        }

        /* Not an identifier lead (handled, with its own guard, just
         * above): digit/'.'/'-'/'+'-led path scanning stays unconditional
         * -- a digit/operator run is typically only 1-4 bytes before
         * failing path_char, unlike an identifier's potentially much
         * longer run, so guarding it isn't worth the extra complexity. */
        {
            size_t plen = try_scan_path(src, i);
            if (plen > 0) {
                size_t end = i + plen;
                if (at_dollar_brace(src, end)) {
                    mode_push(lx, MODE_PATH);
                    mode_push(lx, MODE_NORMAL);
                    lx->pos = end + 2;
                    lx->pending = mk_tok(TOK_DOLLAR_LBRACE, end, end + 2);
                    lx->has_pending = 1;
                    return mk_tok(TOK_PATH, i, end);
                }
                lx->pos = end;
                return mk_tok(TOK_PATH, i, end);
            }
        }

        if (c == '<') {
            /* `<nixpkgs>`-style NIX_PATH search-path literal -- but only
             * when the lookahead actually matches `<name...>`; `<` is also
             * the less-than comparison operator (merged separately, see
             * the two-char-operator block below), so a non-matching `<`
             * must fall through to ordinary operator lexing instead of
             * erroring here. */
            size_t start = i + 1;
            size_t k = start;
            while (path_char(src[k]) || src[k] == '/') k++;
            if (k > start && src[k] == '>') {
                lx->pos = k + 1;
                return mk_tok(TOK_SEARCH_PATH, start, k);
            }
        }

        if (cc_digit(c)) {
            size_t start = i;
            while (cc_digit(src[i])) i++;
            size_t int_len = i - start;
            /* Real Nix's FLOAT grammar (~/nix's src/libexpr/lexer.l):
             *   (([1-9][0-9]*\.[0-9]*)|(0?\.[0-9]+))([Ee][+-]?[0-9]+)?
             * i.e. a float literal ALWAYS needs a literal '.' somewhere --
             * there is no bare-exponent float like "1e10" (that lexes as
             * INT 1 then IDENT e10, NOT a float). Applied to the digit run
             * just scanned: a run starting with a nonzero digit may be
             * followed by '.' and ANY number (including zero) of further
             * digits ("10." is a valid float with zero fractional
             * digits); a run that's the single digit "0" may be followed
             * by '.' only if at least one digit follows the dot ("0." is
             * NOT a float, that lexes as INT 0 then DOT); a multi-digit
             * run starting with '0' (e.g. "00.5") matches neither
             * alternative and is left as plain INT "00" followed by
             * ordinary DOT/INT lexing. */
            int is_float = 0;
            if (src[i] == '.') {
                if (int_len == 1 && src[start] == '0') {
                    if (cc_digit(src[i + 1])) is_float = 1;
                } else if (src[start] != '0') {
                    is_float = 1;
                }
            }
            if (is_float) {
                i++; /* consume '.' */
                scan_float_tail(src, &i);
                lx->pos = i;
                return mk_tok(TOK_FLOAT, start, i);
            }
            lx->pos = i;
            return mk_tok(TOK_INT, start, i);
        }

        /* A float literal with no leading digit at all -- the `\.[0-9]+`
         * shape of real Nix's `0?\.[0-9]+` alternative (a bare ".5" is
         * valid, equal to "0.5"). Checked before the ELLIPSIS/plain-DOT
         * handling below, since this starts with the same '.' character. */
        if (c == '.' && cc_digit(src[i + 1])) {
            size_t start = i;
            i++; /* consume '.' */
            scan_float_tail(src, &i);
            lx->pos = i;
            return mk_tok(TOK_FLOAT, start, i);
        }

        if (c == '"') {
            i++;
            mode_push(lx, MODE_STRING);
            lx->pos = i;
            return mk_tok(TOK_STR_START, tok_start, i);
        }

        if (c == '\'' && src[i + 1] == '\'') {
            i += 2;
            /* \'\'(\ *\n)? -- swallow an immediate (optionally space-padded)
             * newline right after the opening delimiter; it contributes
             * nothing to the string's content or its indentation
             * computation. Only consumed if a newline actually follows the
             * spaces -- otherwise none of it is consumed (matches the `?`
             * on the whole group, not on the spaces alone). */
            size_t j = i;
            while (src[j] == ' ') j++;
            if (src[j] == '\n') i = j + 1;
            mode_push(lx, MODE_ISTRING);
            lx->pos = i;
            return mk_tok(TOK_ISTR_START, tok_start, i);
        }

        /* `${expr}` directly in normal-mode code (not inside a string) --
         * real Nix's dynamic attribute syntax, `set.${name}` (checked
         * before `{` below, which it would otherwise shadow). Reuses the
         * exact same TOK_DOLLAR_LBRACE/mode-push-MODE_NORMAL mechanism
         * string interpolation already uses (lex_dquote_run/
         * lex_indented_run above): the matching '}' pops back out via the
         * ordinary '}' handling just below, same as any other nested
         * '{'. */
        if (c == '$' && src[i + 1] == '{') {
            i += 2;
            mode_push(lx, MODE_NORMAL);
            lx->pos = i;
            return mk_tok(TOK_DOLLAR_LBRACE, tok_start, i);
        }

        if (c == '{') {
            i++;
            mode_push(lx, MODE_NORMAL);
            lx->pos = i;
            return mk_tok(TOK_LBRACE, tok_start, i);
        }
        if (c == '}') {
            if (lx->modes_len == 1) {
                /* See Lexer's own `lenient` field comment: for a lenient
                 * (marker-reparse) Lexer, this is expected -- just an
                 * outer construct's own closer surfacing as this
                 * expression's unavoidable one-token-of-lookahead
                 * overread, never actually consumed by anything. Return
                 * it as an ordinary (if slightly fictitious) TOK_RBRACE
                 * rather than popping a mode that was never pushed. */
                if (lx->lenient) {
                    i++;
                    lx->pos = i;
                    return mk_tok(TOK_RBRACE, tok_start, i);
                }
                fprintf(stderr, "ll: unmatched '}'\n");
                exit(1);
            }
            i++;
            mode_pop(lx);
            lx->pos = i;
            return mk_tok(TOK_RBRACE, tok_start, i);
        }

        /* `...` (lambda-pattern ellipsis) is checked before any single- or
         * double-character operator, same lookahead style as the two-char
         * operators below. */
        if (c == '.' && src[i + 1] == '.' && src[i + 2] == '.') {
            lx->pos = i + 3;
            return mk_tok(TOK_ELLIPSIS, tok_start, tok_start + 3);
        }

        /* Two-character operators are checked before falling back to their
         * single-character meaning (or to "unrecognized character" for
         * '&'/'|'/'/' alone, none of which asnix gives a standalone
         * meaning to -- real Nix doesn't either, except '/' as part of a
         * path literal, not supported yet). `{`/`}` are handled earlier
         * (mode-stack-aware, see above), so they never reach this switch. */
        TokKind kind;
        size_t width = 1;
        if (c == '=' && src[i + 1] == '=') { kind = TOK_EQEQ; width = 2; }
        else if (c == '!' && src[i + 1] == '=') { kind = TOK_NEQ; width = 2; }
        else if (c == '<' && src[i + 1] == '=') { kind = TOK_LE; width = 2; }
        else if (c == '>' && src[i + 1] == '=') { kind = TOK_GE; width = 2; }
        else if (c == '&' && src[i + 1] == '&') { kind = TOK_ANDAND; width = 2; }
        else if (c == '|' && src[i + 1] == '|') { kind = TOK_OROR; width = 2; }
        else if (c == '/' && src[i + 1] == '/') { kind = TOK_SLASHSLASH; width = 2; }
        else if (c == '-' && src[i + 1] == '>') { kind = TOK_ARROW; width = 2; }
        else if (c == '+' && src[i + 1] == '+') { kind = TOK_PLUSPLUS; width = 2; }
        else {
            switch (c) {
                case '+': kind = TOK_PLUS; break;
                case '-': kind = TOK_MINUS; break;
                case '*': kind = TOK_STAR; break;
                case '/': kind = TOK_SLASH; break; /* reached only when both try_scan_path
                                                     * (above) and the '//' two-char check
                                                     * just above already ruled this '/' out
                                                     * as a path literal or update operator. */
                case '(': kind = TOK_LPAREN; break;
                case ')': kind = TOK_RPAREN; break;
                case '=': kind = TOK_EQUALS; break;
                case ';': kind = TOK_SEMI; break;
                case ':': kind = TOK_COLON; break;
                case '.': kind = TOK_DOT; break;
                case '[': kind = TOK_LBRACKET; break;
                case ']': kind = TOK_RBRACKET; break;
                case '<': kind = TOK_LT; break;
                case '>': kind = TOK_GT; break;
                case '!': kind = TOK_BANG; break;
                case '?': kind = TOK_QUESTION; break;
                case ',': kind = TOK_COMMA; break;
                case '@': kind = TOK_AT; break;
                default:
                    fprintf(stderr, "ll: unrecognized character '%c' in source\n", c);
                    exit(1);
            }
        }
        lx->pos = tok_start + width;
        return mk_tok(kind, tok_start, tok_start + width);
    }
}

/* The parser's own on-demand lexing driver, a true pull interface --
 * `pending` first (a step that produced two tokens at once stashed its
 * second one here), else dispatch on the current mode and delegate to
 * the one step function that owns it. MODE_NORMAL is tested first since
 * it's true for the overwhelming majority of real tokens (string/path
 * literal content is a small minority of any real Nix file). */
Token lexer_next(Lexer *lx) {
    if (lx->has_pending) {
        lx->has_pending = 0;
        return lx->pending;
    }
    LexMode mode = mode_top(lx);
    if (mode == MODE_NORMAL) return lex_normal(lx);
    if (mode == MODE_STRING) return lex_dquote_run(lx);
    if (mode == MODE_ISTRING) return lex_indented_run(lx);
    return lex_path_run(lx);
}

static Lexer *lexer_new_ex(const char *src, int lenient) {
    Lexer *lx = malloc(sizeof(Lexer));
    lx->src = src;
    lx->pos = 0;
    lx->modes = NULL;
    lx->modes_len = 0;
    lx->modes_cap = 0;
    mode_push(lx, MODE_NORMAL);
    lx->has_pending = 0;
    lx->lenient = lenient;
    return lx;
}

Lexer *lexer_new(const char *src) {
    return lexer_new_ex(src, 0);
}

/* Marker-reparse addition -- see Lexer's own `lenient` field comment
 * above and parser.c's ll_parse_subexpr (the only caller). */
Lexer *lexer_new_lenient(const char *src) {
    return lexer_new_ex(src, 1);
}

void lexer_free(Lexer *lx) {
    free(lx->modes);
    free(lx);
}

LexerSave lexer_save(Lexer *lx) {
    return (LexerSave){
        .pos = lx->pos, .modes_len = lx->modes_len,
        .pending = lx->pending, .has_pending = lx->has_pending };
}

void lexer_restore(Lexer *lx, LexerSave save) {
    lx->pos = save.pos;
    lx->modes_len = save.modes_len; /* `.modes` itself (and its `.modes_cap`) is only ever
                                      * grown, never shrunk/reallocated-away by mode_push, so
                                      * truncating `.modes_len` back down is a complete,
                                      * correct undo -- nothing above the restored length is
                                      * ever read again until a fresh mode_push overwrites it. */
    lx->cur = lx->modes[lx->modes_len - 1]; /* cur-mirror has to be re-synced here too --
                                               * mode_push/mode_pop are the only two places that
                                               * normally keep it in sync, but a restore changes
                                               * modes_len WITHOUT going through either of them,
                                               * so without this line lx->cur would be left at
                                               * whatever it was at save-time's "future" (the
                                               * probed-ahead state), not at the restored position's
                                               * actual top -- a real divergence the very next
                                               * mode_top() call (inside lexer_next) would read. */
    lx->pending = save.pending;
    lx->has_pending = save.has_pending;
}

/* Unused by any real caller (every real caller goes through ll_parse's
 * internal Lexer instead); kept as a standalone, still-tested-by-
 * construction entry point built on lexer_next(). */
Token *ll_lex(const char *src, size_t *out_count) {
    keyword_lookup_selfcheck();
    Lexer *lx = lexer_new(src);
    size_t src_len = strlen(src);
    size_t cap = src_len / 3 + 32, len = 0;
    Token *out = malloc(cap * sizeof(Token));
    for (;;) {
        Token t = lexer_next(lx);
        if (len == cap) {
            cap *= 2;
            out = realloc(out, cap * sizeof(Token));
        }
        out[len++] = t;
        if (t.kind == TOK_EOF) break;
    }
    lexer_free(lx);
    *out_count = len;
    return out;
}
