/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_LEXER_H
#define LL_LEXER_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    TOK_INT,
    TOK_FLOAT, /* M51: a decimal-point-or-exponent numeric literal -- see lexer.c's
                * scan_float_literal for the exact grammar (matches real Nix's
                * lexer.l FLOAT regex precisely, verified against nix-instantiate). */
    TOK_IDENT,
    TOK_TRUE,
    TOK_FALSE,
    TOK_NULL,
    TOK_LET,
    TOK_IN,
    TOK_IF,
    TOK_THEN,
    TOK_ELSE,
    TOK_REC,
    TOK_INHERIT,
    TOK_ASSERT,
    TOK_WITH,
    TOK_OR, /* `expr.attr or default` -- see parser.c's parse_select. Deliberate scope cut
             * (documented in docs/architecture.md): a genuine reserved keyword here, unlike
             * real Nix where `or` is contextually special ("cursed or") but can still be used
             * as an ordinary identifier elsewhere; asnix's `or` can't be used as a plain
             * identifier at all. */
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_EQUALS,
    TOK_SEMI,
    TOK_COLON,
    TOK_DOT,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LBRACKET,
    TOK_RBRACKET,
    /* Operators (comparisons/booleans/update): see docs/architecture.md's
     * "operators" milestone notes for precedence rationale. */
    TOK_EQEQ,    /* == */
    TOK_NEQ,     /* != */
    TOK_LT,      /* < */
    TOK_GT,      /* > */
    TOK_LE,      /* <= */
    TOK_GE,      /* >= */
    TOK_ANDAND,  /* && */
    TOK_OROR,    /* || */
    TOK_BANG,    /* ! */
    TOK_SLASHSLASH, /* // */
    TOK_ARROW,   /* -> (logical implication) */
    TOK_PLUSPLUS, /* ++ (list concatenation) */
    TOK_SLASH,   /* / (integer division) -- only reached when try_scan_path (lexer.c)
                  * already determined this '/' isn't part of a path literal and it's
                  * not the first of a '//' pair either (see lexer.c's main loop). */
    /* Strings: "..." and ''...'' no longer lex to one flat TOK_STRING --
     * "${" inside either kind re-enters normal expression lexing until the
     * matching "}" (TOK_RBRACE, reused -- see ll_lex's mode-stack
     * comment), then resumes string-content lexing. A string literal is
     * now a TOK_STR_START/TOK_ISTR_START, zero or more of (TOK_STR_PART |
     * TOK_DOLLAR_LBRACE ... TOK_RBRACE), and a matching TOK_STR_END/
     * TOK_ISTR_END. */
    TOK_STR_START,     /* '"' opening a double-quoted string */
    TOK_STR_PART,      /* M141: a RAW (undecoded) literal chunk inside "...", escapes
                        * NOT yet applied -- start/end span the exact source bytes between
                        * the opening quote/prior interpolation and the next boundary, as
                        * a plain offset into `src` (never a decoded copy, unlike pre-M141).
                        * See ll_decode_dquote_span's own comment for where/when the
                        * escape decode actually happens. */
    TOK_STR_END,       /* '"' closing a double-quoted string */
    TOK_ISTR_START,     /* "''" opening an indented string */
    TOK_ISTR_PART,      /* M141: a RAW run of plain indented-string content (no escapes --
                         * an escape always ends this run, see TOK_ISTR_ESC), NOT yet
                         * indentation-stripped (that happens once all of one indented
                         * string's parts are collected -- see parser.c's
                         * strip_indentation, ported from real Nix's
                         * ParserState::stripIndentation). Counts toward indentation
                         * measurement -- the TOK_ISTR_PART/TOK_ISTR_ESC kind split IS
                         * this token's own "hasIndentation" bit (pre-M141 carried it as
                         * a separate flag field on one shared kind; folding it into the
                         * kind tag instead needs no extra Token field at all). */
    TOK_ISTR_ESC,       /* M141: exactly one `''$` / `'''` / `''\<c>` escape unit's RAW
                         * source span (3 or 4 bytes, start/end into `src`) -- decoded on
                         * demand by ll_decode_istr_esc. Never counts toward
                         * indentation (see TOK_ISTR_PART above). Deliberate, documented
                         * scope note vs. the pre-M141 lexer: two or more of these back to
                         * back used to be merged into a single accumulated token (an
                         * artifact of the old eager-ByteBuf's "only flush on a category
                         * change" logic); M141 emits one TOK_ISTR_ESC per escape unit
                         * instead, never merged. This is observably identical for every
                         * case actually exercised by this project's own test corpus (the
                         * indentation algorithm's own `!has_indentation` branch never
                         * inspects a non-indentation chunk's text at all, and the final
                         * AstStringPart list's *concatenated value* is unaffected by how
                         * many pieces it's split into) EXCEPT one vanishingly narrow
                         * corner: `stripIndentation`'s "drop a final line that's empty-
                         * or-spaces-only" check looks only at the literal's OVERALL LAST
                         * part -- if that literal's content ends with two or more
                         * back-to-back escape units where the old merged token would have
                         * been in scope for that check and the new split tokens shift
                         * which one is "last", this one specific edge (e.g. a string whose
                         * very final content is `''\<space>''\<space>`, an escaped-space
                         * pair right at the end) could in principle strip differently.
                         * Not reachable by any fixture in this project's own e2e/
                         * nix-conformance/nixpkgs-hash corpus (checked directly -- see
                         * docs/architecture.md M141); flagged here, not swept under the
                         * rug, in case a future real-world file ever hits it. */
    TOK_ISTR_END,       /* "''" closing an indented string */
    TOK_DOLLAR_LBRACE,  /* "${" -- starts an interpolated expression */
    TOK_QUESTION,  /* '?' -- has-attr operator / lambda-pattern default marker */
    TOK_COMMA,     /* ',' -- lambda-pattern formal separator */
    TOK_AT,        /* '@' -- lambda-pattern whole-arg alias (`n@{...}` / `{...}@n`) */
    TOK_ELLIPSIS,  /* '...' -- lambda-pattern "allow extra fields" marker */
    TOK_PATH,        /* bare path literal: ./foo, ../foo, /foo, foo/bar (see lexer.c's
                      * try_scan_path/try_scan_home_path), OR -- M64 -- the FIRST chunk of
                      * an interpolated path literal (`./${foo}`'s "./", `~/${foo}`'s "~/",
                      * ...), in which case this token is immediately followed by
                      * TOK_DOLLAR_LBRACE rather than standing alone; see
                      * TOK_PATH_PART/TOK_PATH_END below. M141: "started with `~`"
                      * (pre-M141's separate `is_home` flag) is now just `src[start] ==
                      * '~'`, derived by the parser instead of carried on the token. */
    TOK_PATH_PART,   /* M64: a literal chunk inside an interpolated path literal, AFTER the
                      * first (`./a-${foo}-b`'s "-b", possibly zero-length between two
                      * adjacent interpolations) -- no escapes exist in path-literal syntax
                      * at all, so (unlike TOK_STR_PART) this always points directly at a
                      * raw source span, decoded or not, same thing. */
    TOK_PATH_END,    /* M64: closes an interpolated path literal -- a boundary marker only,
                      * no payload (mirrors TOK_STR_END). */
    TOK_SEARCH_PATH, /* <nixpkgs>-style NIX_PATH search-path literal */
    TOK_URI, /* bare URL literal (deprecated-but-still-default-accepted real-Nix syntax,
              * e.g. `http://example.com`) -- see lexer.c's try_scan_uri. Carries its
              * full literal text as a raw source span, same as TOK_PATH/TOK_SEARCH_PATH;
              * the parser turns it into a plain (non-interpolated) AST_STRING. */
    TOK_EOF,
} TokKind;

/* M141: shrunk from 56 to 12 bytes (matching ~/gnix/iets/src/syntax/
 * lexer.rs's own `Token`, the design target -- see docs/architecture.md's
 * M141 section for the full before/after). `start`/`end` are byte offsets
 * into the ONE source buffer every Token was ever lexed from (never a
 * pointer into a separate decoded/malloc'd copy, unlike the pre-M141
 * TOK_STR_PART/TOK_ISTR_PART, which each owned a freshly decoded buffer) --
 * this is the "defer literal decoding" half of the design: an int/float
 * literal's actual numeric value, a string/indented-string chunk's escape
 * decoding, and a path literal's `is_home`/`is_search` classification are
 * all now computed exactly once, on demand, by the PARSER at the point it
 * builds the Ast node that needs them (see ll_decode_float_literal/
 * ll_decode_dquote_span/ll_decode_istr_esc below, and parser.c's own
 * call sites) -- never eagerly at lex time, and never more than once per
 * literal occurrence (same total decode work as before, just relocated).
 * `end` doubles as "the token's own end position" for every kind that has
 * no real payload at all (punctuation/keywords) -- `start` alone already
 * serves the old `.pos` field's role (every token's source position) for
 * every kind uniformly, since nothing is ever a decoded-buffer pointer
 * anymore. */
typedef struct {
    TokKind kind;
    uint32_t start;
    uint32_t end;
} Token;

/* Tokenizes the whole source in one pass into a malloc'd array terminated
 * by a TOK_EOF entry (*out_count includes that terminator). Dead code as
 * of M135 (every real caller goes through ll_parse's internal Lexer
 * instead -- see below), kept as a still-correct, still-tested-by-
 * construction (same per-token scanning core every lexer_next() call
 * ultimately runs) standalone entry point, matching this project's own
 * M127/M135 precedent of keeping a correct-but-unused reference
 * implementation rather than deleting a working, independently-useful
 * API. Aborts the process with a message on an unrecognized character --
 * this is a from-scratch minimal-subset lexer, not a full Nix lexer (see
 * docs/architecture.md M4 scope notes). */
Token *ll_lex(const char *src, size_t *out_count);

/* M141: decodes a TOK_FLOAT's raw [start,end) span via strtod, matching
 * real Nix's own lexer.l exactly ("errno = 0; strtod(yytext, 0); if
 * (errno != 0) throw ParseError") -- safe to call UNBOUNDED (no length
 * cap passed to strtod) because the lexer's own scan already found the
 * maximal span this exact grammar matches at this position, and strtod
 * parses the identical grammar (digits, '.', digits, optional
 * [eE][+-]?digits) -- it is provably unable to consume anything past
 * `end` (whatever immediately follows `end` already failed this
 * grammar, or the lexer's own scan would have included it too), so this
 * always stops at exactly the same byte the lexer already decided ends
 * the token, without needing a malloc'd, NUL-terminated, length-bounded
 * copy the way the pre-M141 lexer-time decode did. */
double ll_decode_float_literal(const char *src, uint32_t start, uint32_t end);

/* M141: decodes a TOK_STR_PART's raw [start,end) span (escapes NOT yet
 * applied) into a freshly malloc'd buffer, applying exactly the same
 * backslash-escape rules the pre-M141 lexer applied eagerly (see
 * lexer.c's decode_escape_char) -- \n \r \t map to the control character,
 * every other escaped byte (including \" \\ \$ \') passes through as
 * itself, and any non-escaped byte (including a raw embedded newline,
 * which real Nix allows inside "...") copies straight through. The
 * decoded length is always <= end-start (every escape shrinks by exactly
 * one byte, 2 raw -> 1 decoded), so this mallocs that upper bound
 * directly without trimming afterward -- the same "malloc the provable
 * upper bound, don't bother reallocing down" convention parser.c's own
 * strip_indentation already uses for the exact same reason. *out_len
 * receives the real decoded length (may be less than end-start). */
char *ll_decode_dquote_span(const char *src, uint32_t start, uint32_t end, size_t *out_len);

/* M141: decodes one TOK_ISTR_ESC raw span (exactly one `''$` / `'''` /
 * `''\<c>` escape unit) into `out` (caller-provided, >= 2 bytes) --
 * *out_len is always 1 (`''$` -> "$", `''\<c>` -> one decoded byte) or 2
 * (`'''` -> "''"). Mirrors lexer.c's pre-M141 istr_append_escape exactly,
 * just evaluated later, from the raw span instead of while lexing. */
void ll_decode_istr_esc(const char *src, uint32_t start, char out[2], size_t *out_len);

/* M175: a true pull-based lexer, replacing M135's LexCursor/TokVec
 * (array-backed, grown via push_tok) design entirely -- see
 * docs/architecture.md M175 for the full rationale (modeled directly on
 * `iets`'s own Lexer, ~/gnix/iets/src/syntax/lexer.rs: `pos()`/`set_pos()`
 * plus a stateless-per-call `next_token()` returning a `Token` BY VALUE,
 * no backing array anywhere).
 *
 * `Lexer` is opaque here (its fields -- byte position, the string/path
 * mode stack, and a one-slot pending-token buffer for the rare step that
 * naturally produces two tokens at once, e.g. a string's content chunk
 * immediately followed by its closing delimiter -- are lexer.c-private;
 * nothing outside lexer.c ever needs to read them directly, unlike the
 * old LexCursor, which had to be partially exposed here purely so
 * lexcur_tok's "is it already there" fast path could be `static inline`).
 * `lexer_next()` produces exactly the next Token in source order every
 * call, never re-lexing a byte once produced -- parser.c's own `cur()`/
 * `peek2()` are now plain reads of a 2-slot lookahead buffer it keeps on
 * the `Parser` struct itself (no lexer.c call at all for a repeated peek
 * at the same position), so `lexer_next()` is only ever called once per
 * token actually consumed, not many times per token the way the old
 * `lexcur_tok` fast path needed to tolerate -- no cross-TU-inlining
 * concern here the way M135 found for that design. */
typedef struct Lexer Lexer;

Lexer *lexer_new(const char *src);

/* Marker-reparse addition (parser.c's ll_parse_subexpr, bytecode.c's
 * PTAG_OP_LAZY_MARKER): same as lexer_new, except an unmatched '}' found
 * with nothing left on this Lexer's OWN mode stack to pop is treated as
 * an ordinary (if slightly fictitious) TOK_RBRACE instead of a fatal
 * syntax error -- expected and harmless here, since this Lexer starts
 * reading PARTWAY into a larger buffer whose own enclosing `{`/`${` (if
 * any) it never saw opened, and the one-token-of-lookahead this file's
 * own cur()/peek2() convention always needs can legitimately overread
 * into that enclosing construct's own closer, which the caller
 * (ll_parse_subexpr) always discards unconsumed anyway. An ordinary
 * lexer_new() Lexer has no such excuse -- its mode stack only ever grows
 * from tokens IT ITSELF produced, so the same situation there is a real,
 * fatal syntax error, unchanged from before this addition. */
Lexer *lexer_new_lenient(const char *src);

Token lexer_next(Lexer *lx);
void lexer_free(Lexer *lx);

/* Full resumable state of a Lexer at some point in time -- lets a caller
 * probe one token further ahead than its own lookahead buffer holds and
 * then rewind as if the probe never happened, exactly mirroring `iets`'s
 * own `Lexer::pos()`/`set_pos()` save/probe/rewind pattern. The only user
 * is parser.c's `peek3()` (`is_formals_pattern`'s bounded 1-2-token-ahead
 * `{...}`-vs-formals disambiguation, M158, needs a third token of
 * lookahead beyond the `tok`/`next` buffer parser.c otherwise keeps). */
typedef struct {
    size_t pos;
    size_t modes_len;
    Token pending;
    int has_pending;
} LexerSave;

LexerSave lexer_save(Lexer *lx);
void lexer_restore(Lexer *lx, LexerSave save);

#endif /* LL_LEXER_H */
