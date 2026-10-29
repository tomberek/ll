/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_LEXER_H
#define LL_LEXER_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    TOK_INT,
    TOK_FLOAT,
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
    TOK_OR,
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
    TOK_EQEQ,
    TOK_NEQ,
    TOK_LT,
    TOK_GT,
    TOK_LE,
    TOK_GE,
    TOK_ANDAND,
    TOK_OROR,
    TOK_BANG,
    TOK_SLASHSLASH,
    TOK_ARROW,    /* -> (logical implication) */
    TOK_PLUSPLUS, /* ++ (list concatenation) */
    TOK_SLASH,    /* / (integer division) */
    TOK_STR_START,
    TOK_STR_PART,
    TOK_STR_END,
    TOK_ISTR_START,
    TOK_ISTR_PART,
    TOK_ISTR_ESC,
    TOK_ISTR_END,
    TOK_DOLLAR_LBRACE,  /* "${" -- starts an interpolated expression */
    TOK_QUESTION,  /* '?' -- has-attr operator / lambda-pattern default marker */
    TOK_COMMA,     /* ',' -- lambda-pattern formal separator */
    TOK_AT,        /* '@' -- lambda-pattern whole-arg alias (`n@{...}` / `{...}@n`) */
    TOK_ELLIPSIS,  /* '...' -- lambda-pattern "allow extra fields" marker */
    TOK_PATH,
    TOK_PATH_PART,
    TOK_PATH_END,
    TOK_SEARCH_PATH, /* <nixpkgs>-style NIX_PATH search-path literal */
    TOK_URI,
    TOK_EOF,
} TokKind;

typedef struct {
    TokKind kind;
    uint32_t start;
    uint32_t end;
} Token;

Token *ll_lex(const char *src, size_t *out_count);

double ll_decode_float_literal(const char *src, uint32_t start, uint32_t end);

char *ll_decode_dquote_span(const char *src, uint32_t start, uint32_t end, size_t *out_len);

void ll_decode_istr_esc(const char *src, uint32_t start, char out[2], size_t *out_len);

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

typedef struct {
    size_t pos;
    size_t modes_len;
    Token pending;
    int has_pending;
} LexerSave;

LexerSave lexer_save(Lexer *lx);
void lexer_restore(Lexer *lx, LexerSave save);

#endif /* LL_LEXER_H */
