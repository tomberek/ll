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

/* perf: one 256-entry character-classification table, replacing the
 * isalnum()/isalpha()/isdigit()/isspace() calls (each a ctype-table load
 * plus a bit test) chained with up to four more byte-compares that
 * path_char/ident_cont/uri_scheme_char/uri_tail_char below used to run
 * from scratch on every single call. Confirmed via `perf annotate
 * asnix_lex` on a standalone lex-only microbenchmark (all-packages.nix,
 * 300+ iterations): path_char alone (inlined into try_scan_path's
 * per-character scan loop, which runs on every leading character of
 * every identifier/number/operator token, not just real path literals)
 * was ~15-20% of asnix_lex's own self time, and try_scan_uri/
 * uri_scheme_char another ~9-10% -- almost entirely from re-deriving
 * each byte's classification one comparison at a time instead of a
 * single table lookup. Every CC_* bit here mirrors one of this file's
 * EXISTING predicates byte-for-byte (not a new/changed character set --
 * a pure speed change): this project never calls setlocale(), so the
 * "C" locale's ASCII-only isalnum/isalpha/isdigit/isspace behavior these
 * predicates relied on is a fixed fact for the whole process lifetime,
 * safe to precompute once into a table instead of recomputing per call.
 * Verified by an exhaustive 256-value comparison against the original
 * isalnum()-based bodies (every byte 0-255) before this table replaced
 * them -- see this milestone's own commit message for the standalone
 * check used (a throwaway harness, not shipped, since this file has no
 * existing unit-test binary of its own to extend; the real regression
 * net here is this file's own thorough use by every test in `make
 * test-e2e`/`test-conformance`/`test-nix-conformance`, which lex every
 * byte of every fixture through this exact table). */
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

/* Table-backed drop-in replacements for the plain isdigit()/isspace()/
 * isalpha() calls this file used at every digit-run/trivia/URI-entry
 * check -- same CHAR_CLASS table as path_char/ident_cont/uri_scheme_char/
 * uri_tail_char above, see this file's own top-of-file comment. */
static int cc_digit(char c) { return (CHAR_CLASS[(unsigned char)c] & CC_DIGIT) != 0; }
static int cc_space(char c) { return (CHAR_CLASS[(unsigned char)c] & CC_SPACE) != 0; }
static int cc_alpha(char c) { return (CHAR_CLASS[(unsigned char)c] & CC_ALPHA) != 0; }

static Token mk_tok(TokKind k, size_t start, size_t end) {
    return (Token){ .kind = k, .start = (uint32_t)start, .end = (uint32_t)end };
}

/* Path-literal scanning (import-paths). Real Nix's lexer recognizes PATH
 * tokens with the (flex) regex `[A-Za-z0-9._+-]*(\/[A-Za-z0-9._+-]+)+\/?`
 * -- an optional prefix of "path chars", followed by one-or-more `/segment`
 * groups, with an optional trailing slash (which real Nix's *parser*, not
 * the lexer, then rejects -- "path has a trailing slash"; checked against
 * nix-instantiate). The `+` group is what makes this unambiguous against
 * every other token in this grammar: nothing else in the language ever
 * uses `/` (there's no division operator), so any occurrence of `/` that
 * isn't inside a string is a path. That's also why this is tried *before*
 * digit/ident scanning below, not after: `foo/bar.nix` must lex as one
 * TOK_PATH, not TOK_IDENT("foo") followed by an unrecognized '/'. */
static int path_char(char c) {
    return (CHAR_CLASS[(unsigned char)c] & CC_PATH_CHAR) != 0;
}

/* True iff src[j..] is exactly "${" -- the interpolation-hole marker
 * (M64). Shared by scan_path_groups below and the main dispatch loop's
 * path-literal cases (plain, absolute, and `~`/home). */
static int at_dollar_brace(const char *src, size_t j) {
    return src[j] == '$' && src[j + 1] == '{';
}

/* Scans zero or more "/PATH_CHAR+" groups starting at *j (which must
 * already be positioned past any PATH_CHAR* prefix -- i.e. the very next
 * decision is "is src[j] a '/'?"), mirroring real Nix's own PATH/HPATH
 * lexer.l grammar's group-loop, generalized with ONE addition: a '/'
 * immediately followed by "${" ends the scan right there (consuming that
 * slash, even though zero path_chars followed it) instead of just
 * stopping -- matching real Nix's PATH_SEG/HPATH_START-before-
 * interpolation special lexer rule (needed for `./${foo}`/`~/${foo}`,
 * M64, see docs/architecture.md). Deliberately does NOT special-case a
 * trailing bare '/' with nothing useful after it (no "${", no further
 * path chars) -- that's left for the caller to optionally consume as the
 * grammar's own separate trailing `/?`, gated on `*saw_group` already
 * being true, so a `//` (update operator) immediately after an
 * as-yet-ungrouped position is never miscounted as a path (critical: if
 * this function consumed an ungrouped trailing '/' unconditionally, "a
 * // b" would mis-lex its first '/' as a 1-char path and its second '/'
 * as another, silently breaking TOK_SLASHSLASH detection). Returns the
 * new position. */
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
 * digit/ident/single-char scanning). M64: also succeeds on a prefix that
 * ends right before "${" (e.g. "./" in "./${foo}", or "/" in "/${foo}"),
 * even though that prefix alone wouldn't otherwise satisfy the "at least
 * one full /PATH_CHAR+ group" rule -- see scan_path_groups. The caller
 * (asnix_lex's main loop) is responsible for noticing that case (checking
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

/* `~`-prefixed ("home") path literal (M64): unlike a plain path, there's
 * no PATH_CHAR* prefix allowed before the first '/' at all -- real Nix's
 * HPATH grammar is exactly `~(/PATH_CHAR+)+/?`, so this only looks for a
 * literal "~/" at src[i] and then reuses scan_path_groups starting right
 * at that '/'. Returns the full token length (including the leading
 * '~'), or 0 if src[i] isn't the start of a valid HPATH (e.g. a bare "~"
 * not followed by '/', or "~/" followed by neither a path char nor
 * "${" -- in which case the caller leaves `i` untouched and falls
 * through to ordinary single-char scanning, same as every other
 * not-actually-a-token probe in this lexer). */
static size_t try_scan_home_path(const char *src, size_t i) {
    if (src[i] != '~' || src[i + 1] != '/') return 0;
    int saw_group = 0;
    size_t j = scan_path_groups(src, i + 1, &saw_group);
    if (!saw_group) return 0;
    if (src[j] == '/') j++; /* optional trailing slash, same rule as try_scan_path */
    return j - i;
}

/* Bare URL literals (deprecated-but-default-accepted real-Nix syntax --
 * `eval-okay-url-literal-default.nix`'s own comment: "By default (no
 * flags), unquoted URL literals are accepted"). Ported directly from
 * real Nix's own lexer.l `URI` regex:
 *   [a-zA-Z][a-zA-Z0-9\+\-\.]*\:[a-zA-Z0-9\%\/\?\:\@\&\=\+\$\,\_\.\!\~\*\'\-]+
 * Tried in the main loop BEFORE ordinary identifier/keyword scanning,
 * same "longest match wins" precedence flex itself gives this rule over
 * ID: e.g. `x:x+1` lexes as ONE URI token "x:x+1", not as a lambda
 * `x: x+1` -- a well-known real-Nix footgun, verified directly against
 * `nix-instantiate --eval` (`(x:x+1) 5` fails with "attempt to call
 * something which is not a function but a string"). Checked the only
 * two *.nix drivers this project's own derivation-hashing work depends
 * on (d.nix/e.nix) for any whitespace-free `ident:ident` lambda that
 * this might newly misparse -- none found. */
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
    /* M23: now INCLUDES '-' -- real Nix's own identifier regex is
     * `[a-zA-Z_][a-zA-Z0-9_'-]*`, i.e. a hyphen is an ordinary identifier
     * character (not just special attrpath-binding syntax), verified
     * directly against `nix-instantiate --eval`: `let flat-md5 = 1; in
     * flat-md5` succeeds (one identifier), and critically `a-b` (NO
     * surrounding whitespace) is itself one identifier/undefined-variable
     * lookup, NOT `a - b` subtraction -- subtraction only "wins" when at
     * least one side has separating whitespace (`a -b`, `a- b` is actually
     * `a-` as a dangling identifier, `a - b`), by ordinary maximal-munch
     * tokenization (same principle as C's `a+++b`). This was a deliberate
     * M4-era scope cut (previously excluded exactly to dodge this
     * ambiguity in the easy, unambiguous way) but real-world Nix source
     * uses hyphenated attribute names constantly (`drv-fixed-output.nix`'s
     * `flat-md5`, nixpkgs's own package/attribute names) -- found blocking
     * numtide/nix-conformance's eval-okay-drv-fixed-output.nix. No existing
     * asnix fixture relied on whitespace-free `a-b` meaning subtraction
     * (checked: every existing `-` arithmetic use already has at least one
     * space), so this is a pure feature add, not a documented-behavior
     * change to anything previously exercised. */
    return (CHAR_CLASS[(unsigned char)c] & CC_IDENT_CONT) != 0;
}

typedef struct {
    const char *kw;
    uint8_t kwlen;
    TokKind kind;
} Keyword;

/* perf: `kwlen` is `sizeof(lit) - 1` (the string literal's own compile-time
 * length, minus its NUL), computed once here instead of via a runtime
 * `strlen(KEYWORDS[k].kw)` inside the lookup loop below -- that loop runs
 * once per keyword-table-entry for EVERY identifier-shaped token in the
 * source (the single most common token shape in any real Nix file), so a
 * redundant strlen there multiplies out badly. Same "carry a known length
 * instead of re-deriving it from a NUL terminator" fix as M141's Token
 * shrink and M143's regex cache, just applied to this fixed, compile-time-
 * known table instead of a runtime string. */
#define KW(lit, k) { lit, sizeof(lit) - 1, k }

static const Keyword KEYWORDS[] = {
    KW("let", TOK_LET),         KW("in", TOK_IN),     KW("if", TOK_IF),
    KW("then", TOK_THEN),       KW("else", TOK_ELSE),  KW("true", TOK_TRUE),
    KW("false", TOK_FALSE),     KW("null", TOK_NULL),
    KW("rec", TOK_REC),         KW("inherit", TOK_INHERIT),
    KW("assert", TOK_ASSERT),   KW("with", TOK_WITH),
    KW("or", TOK_OR),
};

/* perf: the identifier-vs-keyword check below used to scan all 13
 * `KEYWORDS` entries, in declaration order, for EVERY identifier-shaped
 * token in the source -- by far the most common token shape in any real
 * Nix file, and one whose volume scales with a file's own identifier
 * count, not its keyword count (keywords are always a small, bounded
 * fraction). On a file with an unusually large identifier volume (e.g.
 * `hackage-packages.nix`'s ~19,261 inline `callPackage` lambdas, each
 * with its own handful of dependency-name formals -- "aeson", "base",
 * "bytestring", etc., none of them keywords), that 13-entry scan runs in
 * full (minus whatever early-outs the length check buys) on every single
 * one of them. iets's own equivalent (`Lexer::keyword`, a Rust `match` on
 * byte-string literals) compiles to a length-first dispatch, not a linear
 * scan -- this mirrors that shape by hand: switch on `len` first (every
 * `KEYWORDS` entry has a fixed, small length, 2 through 7), so an
 * identifier whose length isn't one of those six values short-circuits to
 * TOK_IDENT with zero comparisons at all, and one whose length DOES match
 * only compares against the (always small, 1-5-entry) subset of keywords
 * sharing that length -- never all 13.
 *
 * Correctness: exhaustively mirrors `KEYWORDS` above (same 13 literals,
 * same TokKind mapping, same "first match wins" semantics, which happens
 * to be moot here since no two keywords share both length AND content).
 * This table is small and stable enough that keeping the two in sync by
 * hand (instead of generating this switch from KEYWORDS at compile time)
 * is the simpler-to-verify option -- any future edit to KEYWORDS must
 * mirror it here, and the existing keyword-coverage tests (every keyword
 * token already exercised by tests/e2e and tests/conformance) catch a
 * drift immediately: an un-mirrored new keyword lexes as TOK_IDENT, a
 * mismatched TokKind mis-parses, either way a test fails loudly. */
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

/* Runs once per process (not once per file/call): verifies `keyword_lookup`
 * agrees with `KEYWORDS` on every entry, so the hand-mirrored switch above
 * can never silently drift from the canonical table -- cheap enough (13
 * entries) to just always run, same "verify, don't just assert by
 * comment" spirit as this project's own M142/M138 correctness passes. */
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
 * actually mean "resume string-content lexing" -- the asm/dispatch loop
 * terminology doesn't apply here, this is purely a lexer-internal stack.
 *
 * M175: this mode stack, the byte position, and a one-slot pending-token
 * buffer (see lexer.h's own Lexer comment) are now ALL the state a `Lexer`
 * carries -- no backing Token array (TokVec) at all, unlike M135's
 * LexCursor. The pending slot exists because a handful of scan steps
 * below naturally produce TWO tokens before there's anywhere sensible to
 * pause (e.g. a string's content chunk immediately followed by its own
 * closing delimiter or a "${" boundary, where the delimiter/boundary
 * token's own mode-stack push/pop has to happen atomically with it being
 * produced) -- `lexer_next()` hands back the first of the two and stashes
 * the second for the very next call, instead of returning both from one
 * call the way `push_tok`-into-an-array used to allow. */
typedef enum { MODE_NORMAL, MODE_STRING, MODE_ISTRING, MODE_PATH } LexMode;

struct Lexer {
    const char *src;
    size_t pos;
    LexMode *modes;
    size_t modes_len, modes_cap;
    /* M177: a mirror of modes[modes_len-1] (the only slot mode_top ever
     * reads), kept in sync by mode_push/mode_pop so lex_normal's own
     * per-token mode_top() call becomes a single field load instead of a
     * 3-load chain (modes_len, then the modes pointer, then
     * modes[modes_len-1] itself) -- see mode_push/mode_top/mode_pop below
     * for the invariant this depends on: EVERY mutation of modes/modes_len
     * happens through those three functions, nothing else in this file
     * (or anywhere else -- Lexer is opaque outside lexer.c, see lexer.h)
     * ever touches a Lexer's mode fields directly. */
    LexMode cur;
    Token pending;
    int has_pending;
    /* Marker-reparse addition (parser.c's asnix_parse_subexpr): 0 for
     * every ordinary Lexer (lexer_new), whose mode stack starts empty
     * and only ever grows from THIS SAME Lexer's own `{`/`${` tokens --
     * for that case, reaching a `}` with nothing left to pop (modes_len
     * == 1) really is a genuine syntax error (an actual unmatched '}' in
     * the source). Set to 1 only by lexer_new_lenient, used when lexing
     * starts PARTWAY into a larger buffer whose own enclosing `{`/`${`
     * (if any) this Lexer never saw opened: asnix_parse_subexpr's
     * caller only ever wants ONE expression's worth of real tokens, but
     * the 2-token lookahead this file's own cur()/peek2() convention
     * needs (see lexer_new's own comment) unavoidably fetches one token
     * PAST that expression's own end before parse_expr ever returns --
     * if that lookahead token happens to be the `}` that closes some
     * OUTER construct (an enclosing attrset/rec/string-interpolation
     * this Lexer has no record of), it is NOT a real syntax error, just
     * an artifact of always needing one token of lookahead; the fix is
     * to not crash on it, since asnix_parse_subexpr's caller (compiler.c)
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
    lx->cur = mode; /* M177: the slot just written is the new top */
}
static LexMode mode_top(Lexer *lx) {
    /* M177: used to be lx->modes[lx->modes_len - 1] -- a 3-load chain
     * (modes_len, modes, then the indexed load) paid on EVERY lex_normal
     * iteration, even though mode_push/mode_pop (the only two places that
     * ever change what the top actually is) already know the new top
     * value at the moment they change it. lx->cur mirrors that value
     * exactly, so this is a pure redundant-recomputation removal: same
     * stack, same contents, same output, one load instead of three. */
    return lx->cur;
}
static void mode_pop(Lexer *lx) {
    /* Every push above is paired 1:1 with a pop at a matching "}"/closing
     * quote, driven entirely by well-formed-by-construction scan logic
     * below -- an underflow here would be a lexer bug, not malformed
     * input (malformed `}`-without-`{` is caught before this is ever
     * called, see the MODE_NORMAL '}' case). */
    lx->modes_len--;
    lx->cur = lx->modes[lx->modes_len - 1]; /* M177: popping reveals a new
                                                top -- re-mirror it (this IS
                                                the array read mode_top used
                                                to do every single token;
                                                now it happens only here,
                                                once per actual pop). */
}

static void fatal_unterminated(const char *what) {
    fprintf(stderr, "asnix: unterminated %s\n", what);
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

double asnix_decode_float_literal(const char *src, uint32_t start, uint32_t end) {
    (void)end; /* see this function's own header comment (lexer.h) for why no bound is needed */
    errno = 0;
    double d = strtod(src + start, NULL);
    if (errno == ERANGE) {
        fprintf(stderr, "asnix: invalid float '%.*s'\n", (int)(end - start), src + start);
        exit(1);
    }
    return d;
}

char *asnix_decode_dquote_span(const char *src, uint32_t start, uint32_t end, size_t *out_len) {
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

void asnix_decode_istr_esc(const char *src, uint32_t start, char out[2], size_t *out_len) {
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
 * M141: the TOK_STR_PART pushed here is a RAW span -- escapes are
 * scanned over (to find where the span ends and to validate there's no
 * unterminated trailing backslash) but never decoded; decoding happens
 * later, on demand, via asnix_decode_dquote_span (see lexer.h). Verified
 * against real `nix-instantiate` for raw embedded newlines (allowed,
 * unlike this lexer's pre-interpolation M5 behavior -- see
 * docs/architecture.md's Known Bugs list) and the \n \r \t \" \\ \$
 * escapes. */
/* M175: produces exactly ONE Token per call -- a content chunk still
 * returns first; whenever this step's own boundary token (TOK_STR_END or
 * TOK_DOLLAR_LBRACE) is ready at the SAME time (the mode-stack change has
 * to happen atomically with it), that second token is stashed in
 * `lx->pending` for the very next call to hand back immediately, instead
 * of both being pushed into an array in one go (see lexer.h's own Lexer
 * comment). `lx->pos` always ends up at the position reflecting BOTH
 * tokens already consumed, even though only the first is returned here --
 * the pending slot alone is what makes the second one still show up, in
 * order, on the next call. */
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
        /* M-dollar-pair: "$$" is always a literal, atomically-consumed
         * pair -- confirmed against real nix-instantiate: "a$${foo}b"
         * passes through unchanged (length 9, no lookup of `foo` at
         * all, even when `foo` is undefined), while an ODD run of
         * dollars ("a$$${foo}b") leaves exactly one unpaired "$" that
         * DOES start real interpolation. Must be checked before the
         * "${"-starts-interpolation rule below, and must `continue`
         * rather than fall into it -- otherwise the second "$" of a
         * literal pair gets misread as the start of a fresh "${"
         * check against whatever follows it. Found via a false-
         * positive "undefined variable" on real nixpkgs code using
         * this exact idiom (accounts-qt's `$${INSTALL_PREFIX}`,
         * a literal qmake variable reference, not a Nix one). */
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
 * closes the string. M141: both TOK_ISTR_PART and TOK_ISTR_ESC carry a
 * RAW span only -- decoding (trivial for ISTR_PART: none needed at all,
 * it's already plain source text; a small fixed-shape decode for
 * TOK_ISTR_ESC) happens on demand, see asnix_decode_istr_esc. See
 * lexer.h's own TOK_ISTR_ESC comment for the one narrow, documented
 * behavioral difference from the pre-M141 lexer (back-to-back escape
 * units used to merge into one token; M141 never merges).
 *
 * Deliberate, documented scope cut vs. real Nix's lexer: a lone "'"
 * immediately followed by "$" is treated here as two ordinary content
 * characters (real Nix special-cases it as a hasIndentation=false
 * boundary token). This only affects the indentation-*counting* boundary
 * in the vanishingly rare case of a leading "'$" run of whitespace, never
 * the produced string's actual bytes -- not exercised by any case in
 * tests/e2e, and not worth the extra complexity here. */
/* M175: produces exactly one Token per call, same pending-slot convention
 * as lex_dquote_run above for the two unconditional-pair boundaries
 * (closing "''" and "${"). The escape case (''$ / ''' / ''\<c>) is
 * handled WITHOUT the pending slot at all, by a different trick: when a
 * nonempty content run precedes the escape, this returns just the
 * TOK_ISTR_PART chunk and -- critically -- leaves `lx->pos` sitting right
 * BEFORE the escape's own bytes (not past them), rather than consuming it
 * here. The very next call re-enters this same function fresh with
 * `content_start == lx->pos` (recomputed at entry, see below) landing
 * exactly on that same escape; this time `i > content_start` is false (no
 * unflushed content ahead of it), so it falls straight to the
 * `lx->pos = esc_end; return TOK_ISTR_ESC` branch instead of looping back
 * into content-scanning -- i.e. the SAME escape is "re-detected", not
 * re-decoded or duplicated, by ordinary re-entry rather than a stashed
 * second token. Safe only because content_start is always freshly derived
 * from lx->pos at this function's own top, never carried across calls any
 * other way -- true both before and after this milestone. */
static Token lex_indented_run(Lexer *lx) {
    const char *src = lx->src;
    size_t content_start = lx->pos;
    size_t i = lx->pos;

    for (;;) {
        char c = src[i];
        if (c == '\0') fatal_unterminated("indented string literal (''...'')");

        /* M-dollar-pair: see lex_dquote_run's own comment on the
         * identical rule -- "$$" is always a literal, atomically-
         * consumed pair, checked before "${"-starts-interpolation so
         * the second "$" of a pair never gets re-read as the start of
         * a fresh "${" check. */
        if (c == '$' && src[i + 1] == '$') {
            i += 2;
            continue;
        }
        if (c == '$' && src[i + 1] == '{') {
            /* Always emit a (possibly empty) PART before the boundary --
             * matches the pre-M141 istr_flush_always convention. */
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
                /* Flush the pending content run only if nonempty -- matches
                 * the pre-M141 istr_flush_if_nonempty convention (a
                 * content-to-escape category change doesn't force an empty
                 * PART the way a real boundary does). See this function's
                 * own header comment: returning here WITHOUT consuming the
                 * escape (lx->pos left at the escape's own start) is what
                 * makes the next call re-detect and emit it alone. */
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
         * perf: jump ahead to the next CANDIDATE boundary byte ('$' or
         * '\'') via strchrnul instead of advancing one byte at a time --
         * same "jump then verify" technique the comment-skip fix in
         * lex_normal already uses, and the same safety contract
         * (strchrnul never reads past the string's own NUL terminator).
         * Searches from i+1, not i: the byte at i, if it IS '$' or '\'',
         * already failed the real-boundary checks above (a lone '$' not
         * followed by '{', or a lone '\'' not followed by another
         * '\''), so re-finding it at the same position would never
         * advance. A landed-on candidate that turns out to be another
         * false positive just falls through these same checks again at
         * the top of the loop, exactly as a plain byte would. */
        const char *dollar = strchrnul(src + i + 1, '$');
        const char *squote = strchrnul(src + i + 1, '\'');
        i = (size_t)((dollar < squote ? dollar : squote) - src);
    }
}

/* M64: scans one run of path-literal content starting at *i (positioned
 * just after a closed "${ ... }" interpolation inside a path literal
 * whose FIRST chunk already established it continues past one -- see the
 * MODE_PATH push sites in asnix_lex's main loop above), up to the next
 * boundary: either another "${" or the first character that isn't a
 * path continuation at all (ending the whole literal). Pushes exactly
 * one TOK_PATH_PART (the literal chunk just scanned, possibly
 * zero-length -- e.g. between two adjacent interpolations, same
 * allowance lex_dquote_run makes for TOK_STR_PART) followed by the
 * boundary token (TOK_DOLLAR_LBRACE or TOK_PATH_END). Unlike
 * lex_dquote_run/lex_indented_run, there is no escape processing at all
 * in path-literal syntax (no backslash is ever a path_char), so this
 * chunk's span already points straight at the final text -- no decode
 * step needed downstream at all, same as before M141. A chunk may start
 * with plain path_chars, a '/'-group run, or both, exactly like a fresh
 * path literal's own scan (reuses scan_path_groups the same way
 * try_scan_path does), EXCEPT it's never required to contain any
 * group at all (a bare trailing `-b`-style run with no slash, like
 * `./a-${foo}-b`'s "-b", is perfectly valid here -- only the very
 * FIRST chunk of a path literal needs that requirement, to tell a path
 * apart from "not a path at all"). */
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

/* M141: shared tail-scan for a float literal already known to match
 * (either a digit run followed by '.', or a bare '.' followed by a
 * digit) -- see lex_until's two call sites below. `*i` arrives already
 * positioned just past the decisive '.'; this scans the fractional
 * digits and an optional exponent ONLY (no strtod call here anymore --
 * that's deferred to asnix_decode_float_literal, called later from
 * parser.c once an AST_FLOAT node is actually being built). Mirrors real
 * Nix's own lexer.l FLOAT regex exactly, same as the pre-M141 version of
 * this function (renamed from scan_float_tail_and_parse, which also did
 * the strtod+malloc+errno-check this split moved out). */
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

/* M175: replaces M135's `lex_until` (which pushed every produced token
 * into a shared TokVec array, stopping once at least `min_len` tokens
 * existed) -- this produces exactly ONE Token per call, by value, never
 * writing into any array at all. Every per-character scanning DECISION
 * below is byte-for-byte identical to the pre-M175 code (nothing about
 * what counts as a token, or where one ends, changed); only the "what do
 * we do once we know a token's span" step changed, from `push_tok(v,
 * ...); continue;` to `lx->pos = ...; return mk_tok(...);` (a direct
 * value return instead of an array append). The handful of steps that
 * used to push TWO tokens in one go (a path/URI chunk immediately
 * followed by a "${" interpolation boundary) stash the second one in
 * `lx->pending` for the very next call, exactly like lex_dquote_run/
 * lex_indented_run/lex_path_run above -- see lexer.h's own Lexer comment. */
static Token lex_normal(Lexer *lx) {
    const char *src = lx->src;
    for (;;) {
        size_t i = lx->pos;
        char c = src[i];
        size_t tok_start = i; /* byte offset of whatever token (if any) starts here */

        if (c == '\0') {
            if (lx->modes_len != 1) {
                fprintf(stderr, "asnix: unexpected end of input inside '${ ... }'\n");
                exit(1);
            }
            lx->pos = i; /* left unchanged: a repeated call at EOF keeps producing TOK_EOF,
                           * same re-callable convention the old array-based design had
                           * (peek2() at the EOF position used to grow the array by one more
                           * duplicate EOF entry on demand; here it's just another call). */
            return mk_tok(TOK_EOF, i, i);
        }

        if (cc_space(c)) {
            /* perf: skip the whole run of whitespace in one tight loop
             * instead of bouncing back through the outer dispatch (the
             * '\0'/'#'/block-comment/path-literal checks above) once
             * per space character -- real nixpkgs source is mostly
             * indentation and blank lines. cc_space('\0') is false (see
             * CHAR_CLASS above), so this stops at end of input exactly
             * like the outer loop's own '\0' case would. */
            do { i++; } while (cc_space(src[i]));
            lx->pos = i;
            continue;
        }
        if (c == '#') { /* line comment, a harmless real-Nix-ism to allow in fixtures */
            /* perf: strchrnul finds '\n' or the string's own terminating
             * '\0', whichever comes first -- exactly this loop's own
             * stopping condition, in one SIMD-accelerated glibc call
             * instead of a byte-at-a-time scan. Real nixpkgs files are
             * full of line comments (license headers, inline notes). */
            i = (size_t)(strchrnul(src + i, '\n') - src);
            lx->pos = i;
            continue;
        }
        if (c == '/' && src[i + 1] == '*') { /* block comment -- real Nix files are full of these
                                               * (e.g. nixpkgs's pkgs/top-level/impure.nix opens with
                                               * one); previously unhandled entirely, so the lexer hit
                                               * "unrecognized character '/'" on the literal '/*' the
                                               * moment any real-world file was fed in -- found
                                               * evaluating real nixpkgs via d.nix, see
                                               * docs/architecture.md's Known Bugs list. Checked before
                                               * try_scan_path below: '/*' never matches a path literal
                                               * anyway ('*' isn't a path_char), but comments should
                                               * never compete with path-literal scanning regardless. */
            i += 2;
            /* perf: same strchrnul idea as the line comment above, one
             * level removed -- jump straight to the next star (or the
             * terminating NUL) instead of checking every byte by hand,
             * then verify it is really the closing star-slash and not
             * just a star inside the comment's own text; if not, skip
             * past it and keep searching. Still stops in the identical
             * state (at the closing delimiter's own star, or at NUL) as
             * the byte-at-a-time loop this replaces. */
            for (;;) {
                i = (size_t)(strchrnul(src + i, '*') - src);
                if (!src[i] || src[i + 1] == '/') break;
                i++;
            }
            if (!src[i]) {
                fprintf(stderr, "asnix: unterminated block comment\n");
                exit(1);
            }
            i += 2;
            lx->pos = i;
            continue;
        }

        /* Tried before digit/string/single-char below -- see
         * try_scan_path's comment for why this has to come first. M64:
         * `~`-prefixed home paths are tried alongside (src[i]=='~' is
         * never itself a path_char, so try_scan_path can never fire at
         * this position on its own -- a separate probe is required). Both
         * branches below handle the "immediately followed by ${" case
         * identically: emit just this chunk as TOK_PATH (its `~`-ness is
         * recovered later by the parser directly from `src[start]`, see
         * lexer.h -- no is_home flag carried here anymore), then push
         * MODE_PATH (so the matching '}' that closes the interpolation's
         * expression returns here, not to plain MODE_NORMAL) underneath a
         * fresh MODE_NORMAL for the expression itself, and emit
         * TOK_DOLLAR_LBRACE -- exactly the same push-two-modes-and-emit-
         * the-boundary-token shape lex_dquote_run/lex_indented_run already
         * use for "${" inside a string.
         *
         * M138: unlike the general try_scan_path call a few lines below
         * (kept unconditional, see that call site's own comment),
         * try_scan_home_path is left unconditional here too -- it's an
         * instant one-branch bail for any non-'~' leading byte, nothing
         * to guard. */
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

        /* M138: identifier-leading tokens get their own guarded path/URI
         * check instead of falling into the unconditional try_scan_path
         * (and, for alpha leads, try_scan_uri) calls below -- the second
         * lexer.c lever flagged as a follow-on in M129 (see that
         * section's "Checked against iets" paragraph) and in iets's own
         * `next_token` (~/gnix/iets/src/syntax/lexer.rs, the
         * `b'a'..=b'z' | b'A'..=b'Z' | b'_'` match arm): scan the
         * ordinary identifier's own ident_cont run ONCE (needed anyway to
         * produce a plain TOK_IDENT/keyword), then make the expensive
         * path_char-run-plus-group scan (try_scan_path) and URI scan
         * (try_scan_uri) conditional on a cheap single-byte peek at the
         * character immediately following that run, instead of always
         * attempting both scans first and then re-walking the exact same
         * bytes a second time via ident_cont moments later whenever they
         * fail (which is every time for an ordinary identifier/keyword --
         * by far the most common token in real Nix source).
         *
         * Correctness (the edge case M129 flagged and this milestone's
         * commit message verifies): ident_cont and path_char/
         * uri_scheme_char are NOT the same character set. '.' and '+' are
         * path_char/uri_scheme_char but NOT ident_cont -- e.g. a token
         * shaped like "a.b:foo" (a URI with a scheme containing '.') or
         * "foo.bar/baz" (a path literal with '.' in its prefix) needs the
         * real path_char/uri_scheme_char run, which keeps going past
         * where ident_cont already stopped, checked -- not just where the
         * identifier's own run ends. Conversely, '\'' is ident_cont but
         * NOT path_char/uri_scheme_char.
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
         * fail, not merely a fast-path approximation (see M138's own
         * writeup, docs/architecture.md, for the full proof). */
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
             * above by the ident_cont scan this guard needed anyway -- no
             * second ident_cont walk later in this function (see the old
             * ident_start block this replaced, now removed). */
            size_t start = i;
            size_t len = j - start;
            TokKind kind = keyword_lookup(src + start, len);
            lx->pos = j;
            return mk_tok(kind, start, j);
        }

        /* Not an identifier lead (handled, with its own guard, just above):
         * digit/'.'/'-'/'+'-led path scanning stays unconditional, exactly
         * as before this milestone -- iets doesn't bother guarding these
         * either (see its `or_float_or_path`'s own unconditional
         * `scan_path_token` call, and the direct `scan_path_token` calls in
         * its '.'/'-'/'+' match arms), since a digit/operator run is
         * typically only 1-4 bytes before failing path_char, unlike an
         * identifier's potentially much longer run -- not worth the extra
         * branch/complexity for a near-zero win. */
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
            /* M51: real Nix's FLOAT grammar (grepped directly from
             * ~/nix's src/libexpr/lexer.l):
             *   (([1-9][0-9]*\.[0-9]*)|(0?\.[0-9]+))([Ee][+-]?[0-9]+)?
             * i.e. a float literal ALWAYS needs a literal '.' somewhere --
             * there is no bare-exponent float like "1e10" (confirmed:
             * `nix-instantiate --eval` on that input errors "undefined
             * variable 'e10'", i.e. it lexes as INT 1 then IDENT e10, NOT
             * a float). Applied to the digit run just scanned: a run
             * starting with a nonzero digit may be followed by '.' and
             * ANY number (including zero) of further digits ("10." is a
             * valid float with zero fractional digits -- verified
             * against nix-instantiate); a run that's the single digit
             * "0" may be followed by '.' only if at least one digit
             * follows the dot ("0." is NOT a float -- verified: that
             * lexes as INT 0 then DOT, same as before this milestone); a
             * multi-digit run starting with '0' (e.g. "00.5") matches
             * neither alternative and is left as plain INT "00" followed
             * by ordinary DOT/INT lexing, an edge case no sane digit
             * literal hits anyway. */
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

        /* M51: a float literal with no leading digit at all -- the
         * `\.[0-9]+` shape of real Nix's `0?\.[0-9]+` alternative (the
         * "0?" is optional, so a bare ".5" is valid, verified against
         * nix-instantiate: `.5` == `0.5`). Checked before the ELLIPSIS/
         * plain-DOT handling below, since this starts with the same '.'
         * character. */
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
         * '{'. Previously `$` had no meaning at all outside an active
         * string mode, so this hit "unrecognized character '$'" -- found
         * evaluating real nixpkgs (`pkgs/top-level/default.nix` uses
         * `set.${name}`), see docs/architecture.md's Known Bugs list. */
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
                fprintf(stderr, "asnix: unmatched '}'\n");
                exit(1);
            }
            i++;
            mode_pop(lx);
            lx->pos = i;
            return mk_tok(TOK_RBRACE, tok_start, i);
        }

        /* M138: the ident_start(c) branch that used to live here (plain
         * TOK_IDENT/keyword scanning, re-walking ident_cont from scratch)
         * was moved up above, merged into the new guarded path/URI check --
         * ident_start(c) can never reach this point anymore (that earlier
         * branch always `continue`s/`return`s). */

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
                                                     * as a path literal or update operator --
                                                     * see TOK_SLASH's own comment (lexer.h). */
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
                    fprintf(stderr, "asnix: unrecognized character '%c' in source\n", c);
                    exit(1);
            }
        }
        lx->pos = tok_start + width;
        return mk_tok(kind, tok_start, tok_start + width);
    }
}

/* M175: the parser's own on-demand lexing driver, now a true pull
 * interface -- `pending` first (a step that produced two tokens at once
 * stashed its second one here), else dispatch on the current mode and
 * delegate to the one step function that owns it, exactly mirroring
 * `iets`'s own `Lexer::next_token` dispatch (~/gnix/iets/src/syntax/
 * lexer.rs), minus its separate-functions-called-directly-by-the-parser
 * detail (asnix keeps every token, including string/path content chunks,
 * flowing through this single entry point instead, unchanged from M135).
 *
 * M194: the mode compare below now tests MODE_NORMAL FIRST, not last.
 * A follow-up candidate flagged by M182/M183's own investigation was
 * porting iets's structure literally -- having `parser.c` call
 * `lex_normal`/`lex_dquote_run`/`lex_indented_run`/`lex_path_run`
 * directly at each call site, bypassing this dispatcher for the
 * overwhelming majority of calls. Investigated and found NOT safely
 * portable as-is: iets's parser (~/gnix/iets/src/syntax/parser.rs)
 * keeps `tok`/`next` as LAZY `Option<Token>` slots, only calling
 * `next_token()` on demand (`peek()`'s `None` branch) -- its
 * `parse_string_parts`/`parse_ind_string` both `debug_assert!(self.tok.
 * is_none() && self.next.is_none())` on entry specifically because nothing
 * has pre-fetched past the opening delimiter yet. asnix's `Parser.tok`/
 * `.next` (parser.c, M181) are the opposite: an EAGERLY always-full
 * 2-slot buffer, refilled unconditionally by every `advance()` --
 * exactly the design M181 built, and measured as a major win, specifically
 * so `cur()`/`peek2()` could be plain field reads with zero calls into
 * lexer.c at all. Because of that, by the time `parser.c` ever "knows"
 * it's about to read string/path content, the lookahead slot for it has
 * ALREADY been fetched through this same generic dispatcher one
 * `advance()` call earlier -- there is no call site in `parser.c` where
 * skipping this dispatcher doesn't also mean re-deriving, per call site,
 * which of the 4 lexer step functions that fetch is secretly going to
 * need (a correctness-sensitive, whole-file audit of every `advance()`/
 * `expect()` site, the exact kind of change this file's own fragility
 * history -- 9+ prior milestones -- argues against for an estimated
 * ~0.2-0.3% win). What IS safe and captures the same flagged cost without
 * touching `parser.c`, the pending-slot convention, or the eager-lookahead
 * invariant at all: `mode_top(lx) == MODE_NORMAL` is true for the
 * overwhelming majority of real tokens (string/path literal content is a
 * small minority of any real Nix file) yet was tested LAST, after two
 * guaranteed-false compares against MODE_STRING/MODE_ISTRING on every
 * single ordinary token. Reordering to test it first turns the common
 * case into exactly one compare instead of three, with the mode-stack
 * semantics, pending-buffer contract, and every byte of per-mode scanning
 * logic completely unchanged -- a pure dispatch-order fix, not a
 * structural one. */
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
 * above and parser.c's asnix_parse_subexpr (the only caller). */
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
    lx->cur = lx->modes[lx->modes_len - 1]; /* M177's cur-mirror has to be re-synced here too --
                                               * mode_push/mode_pop are the only two places that
                                               * normally keep it in sync, but a restore changes
                                               * modes_len WITHOUT going through either of them,
                                               * so without this line lx->cur would be left at
                                               * whatever it was at save-time's "future" (the
                                               * probed-ahead state), not at the restored position's
                                               * actual top -- a real divergence the very next
                                               * mode_top() call (inside lexer_next) would read.
                                               * `.modes` is never shrunk (see above), so the slot
                                               * this reads was never overwritten by the probe. */
    lx->pending = save.pending;
    lx->has_pending = save.has_pending;
}

/* Dead code as of M135 (every real caller goes through asnix_parse's
 * internal Lexer instead -- see lexer.h's own comment); kept as a still-
 * correct, still-tested-by-construction (same lex_normal/lex_dquote_run/
 * lex_indented_run/lex_path_run core every lexer_next call ultimately
 * runs) standalone entry point, matching this project's own M127/M135
 * precedent of keeping a correct-but-unused reference implementation
 * rather than deleting a working, independently-useful API. M175: re-
 * implemented in terms of the new pull-based lexer_next() -- this
 * function's own small, local, malloc'd-and-grown array is now the ONLY
 * place in this file that assembles tokens into an array at all (every
 * real, in-use caller never materializes one). */
Token *asnix_lex(const char *src, size_t *out_count) {
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
