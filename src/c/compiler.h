/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef ASNIX_COMPILER_H
#define ASNIX_COMPILER_H

#include <stddef.h>

#include "ast.h"
#include "asnix/ir.h"

/* Compiles an AST produced by asnix_parse() into a linked (handler
 * addresses already resolved) IrProgram ready for asnix_eval(). Name
 * resolution happens inline during this walk (no separate annotation
 * pass) -- see src/c/compiler.c for why that's sufficient at M4's scope.
 *
 * `base_dir` is the absolute directory of the .nix file being compiled
 * (no trailing slash) -- relative path literals (AST_PATH, e.g.
 * `./foo.nix`) resolve against it, matching real Nix's rule that a path
 * literal resolves relative to the directory of the file it's *written
 * in*, not the process's cwd or the importing file's directory (see
 * src/c/eval_file.c, which computes this once per file and is the only
 * other caller of asnix_compile besides this file's own tests).
 *
 * `file_path` (M82): the resolved absolute path of the .nix file being
 * compiled (not just its directory) -- used ONLY for `__curPos`'s `file`
 * field (AST_CURPOS in compile_expr), so a `__curPos` token inside an
 * IMPORTED file reports that file's own path, not the outermost file
 * asnix was invoked on (the old M80 approximation). May be NULL (falls
 * back to `asnix_eval_top_level_file()`, matching the old behavior
 * exactly) -- every real caller passes it, NULL only covers this file's
 * own unit tests, which don't exercise `__curPos`. */
IrProgram *asnix_compile(Ast *root, const char *base_dir, const char *file_path);

/* M54: `builtins.scopedImport`'s compile-time half -- same as
 * asnix_compile above, but with one extra named (lexical, not `with`-
 * style fallback) ScopeFrame of `extra_count` names (`extra_names`/
 * `extra_lens`) spliced in between the ordinary root `builtins` frame
 * and `root`'s own top-level expression, so each of those names resolves
 * (and takes priority over any bare global/builtins.X of the same name)
 * like any other variable reference. See compiler.c's own comment on
 * this function and eval_file.c's asnix_eval_file_scoped (the only
 * caller) for the matching runtime half. */
IrProgram *asnix_compile_scoped(Ast *root, const char *base_dir, const char *file_path,
                                 const char **extra_names, size_t *extra_lens, size_t extra_count);

#endif /* ASNIX_COMPILER_H */
