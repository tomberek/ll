/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_CODEGEN_H
#define LL_CODEGEN_H

#include "ast.h"
#include "ll/value.h"

/* Compiles an AST directly to a buffer of real x86-64 machine code
 * (mmap'd PROT_EXEC). Scope: literals, arithmetic (+ - *), `if`/`then`/
 * `else`, and `let`/`in` (bound values live as plain native-stack slots,
 * addressed by a compile-time-tracked %rsp offset -- no heap Env since
 * this subset has no closures). Function application/lambdas are NOT
 * supported -- ll_codegen() aborts the process with a clear message if
 * it encounters an AST_LAMBDA/AST_APP node.
 *
 * The returned function is an ordinary SysV-ABI-compliant `Value (*)(void)`
 * -- callable directly, no special entry convention. */
typedef Value (*LlNativeFn)(void);

LlNativeFn ll_codegen(Ast *root);

#endif /* LL_CODEGEN_H */
