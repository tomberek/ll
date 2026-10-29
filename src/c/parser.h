/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_PARSER_H
#define LL_PARSER_H

#include "ast.h"
#include "lexer.h"

/* Recursive-descent parser for the M4 minimal subset plus the "operators"
 * milestone's additions: int/bool/null literals, arithmetic (+ - *),
 * comparisons (== != < > <= >=), booleans (&& || !, short-circuit),
 * attrset update (//), `assert cond; body`, function application by
 * juxtaposition, single-param lambdas (`a: body`, curried for multiple
 * params exactly like real Nix), `let`/`in`, and `if`/`then`/`else`.
 * Aborts the process with a message on a syntax error.
 *
 * `src`: M80 (see docs/architecture.md) -- the SAME source buffer every
 * `Token.pos`/`.start` points into, needed ONLY to compute a real
 * line/column for `__curPos` (by counting newlines from `src` up to the
 * token's own `.pos` -- see parser.c's curpos_line_col). Every other
 * token field already carries everything the rest of the parser needs
 * without ever touching `src` directly, so this one extra parameter is
 * the full cost of that one feature -- NOT the start of a broader
 * "thread position through every AST/IR node" change
 * (`builtins.unsafeGetAttrPos`'s real per-ATTRIBUTE position remains the
 * disproportionate, deliberately out-of-scope one -- see
 * builtins_m54.c's own `unsafeGetAttrPos2_impl` comment).
 *
 * M135: lexes `src` on demand internally (via a LexCursor, lexer.h) --
 * callers no longer call ll_lex() themselves first; see
 * docs/architecture.md M134/M135. */
Ast *ll_parse(const char *src);

/* Marker-reparse addition (see parser.c's own comment on this function):
 * parses exactly one expression starting at `src[start_offset]`, where
 * `src` is a full file buffer an earlier ll_parse(src) call (in a
 * DIFFERENT, now-dead process) already walked once. Used only by
 * compiler.c's ll_compile_lazy_thunk_body, for a LazyThunkDescriptor
 * that was reconstructed from a bytecode-cache entry instead of built
 * live during that file's own original compile. */
Ast *ll_parse_subexpr(const char *src, size_t start_offset);

#endif /* LL_PARSER_H */
