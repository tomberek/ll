/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef ASNIX_CODEGEN_H
#define ASNIX_CODEGEN_H

#include "ast.h"
#include "asnix/value.h"

/*
 * M4.5: copy-and-patch native code generation. Compiles an AST directly to
 * a buffer of real x86-64 machine code (mmap'd PROT_EXEC) instead of the
 * IrOp records M3/M4's threaded interpreter walks -- eliminating per-op
 * dispatch entirely in favor of straight-line fallthrough code with real,
 * CPU-predicted branches.
 *
 * Scope: literals, arithmetic (+ - *), `if`/`then`/`else`, and `let`/`in`
 * (bound values live as plain native-stack slots, addressed by a
 * compile-time-tracked %rsp offset -- no heap Env is needed since this
 * subset has no closures). Function application/lambdas are NOT yet
 * supported here -- they need a heap Closure + an explicit call/return
 * protocol (mirroring src/asm/ops/op_call.S), which is a real additional
 * project, not a small extension; see docs/architecture.md's M4.5 notes
 * for why this was scoped out of the first cut. asnix_codegen() aborts the
 * process with a clear message if it encounters an AST_LAMBDA/AST_APP
 * node.
 *
 * The returned function is an ordinary SysV-ABI-compliant `Value (*)(void)`
 * -- callable directly, no special entry convention.
 */
typedef Value (*AsnixNativeFn)(void);

AsnixNativeFn asnix_codegen(Ast *root);

#endif /* ASNIX_CODEGEN_H */
