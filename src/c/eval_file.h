/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_EVAL_FILE_H
#define LL_EVAL_FILE_H

#include "ll/value.h"

/* Runs the full lex -> parse -> compile -> eval pipeline on the .nix file
 * at `path` and returns the resulting Value. Used by both main() (the
 * top-level program) and the `import` builtin (src/c/builtins.c) so the
 * pipeline exists in exactly one place.
 *
 * Each call gets its OWN fresh root Env/`builtins` attrset (matching real
 * Nix: an imported file does not implicitly see the importing file's
 * local scope, only the global `builtins`). Relative path literals
 * (including the target of a nested `import`) written in `path` resolve
 * against `path`'s OWN directory, not the process's cwd or whichever
 * file imported it; implemented by resolving `path` to an absolute,
 * lexically-canonicalized path (does NOT resolve symlinks on disk,
 * unlike `realpath()` -- see eval_file.c's resolve_import_path for why)
 * and passing its directory to `ll_compile()` as the base_dir every
 * AST_PATH node in this file resolves against.
 *
 * Aborts the process (message to stderr, exit 1) if `path` can't be
 * opened or resolved to an absolute path. */
Value ll_eval_file(const char *path);

/* `builtins.scopedImport scope path` -- same pipeline as ll_eval_file
 * above, except `scope` (an already-forced Attrs Value) supplies an
 * extra, innermost lexical scope level for the imported file: each of
 * `scope`'s attribute names becomes directly resolvable as a bare
 * identifier inside `path`'s own source (compiler.c's
 * ll_compile_scoped), taking priority over (shadowing) the ordinary
 * bare globals/`builtins.X` of the same name. `scope`'s field VALUES
 * are not forced here (same laziness as any other attrset field) --
 * only its NAMES are needed up front, to build the compile-time
 * ScopeFrame. */
Value ll_eval_file_scoped(const char *path, Value scope);

/* Runs the same lex -> parse -> compile -> eval pipeline over a source
 * STRING (not a file) -- used by main.c's `--arg name EXPR` CLI flag,
 * whose EXPR is an arbitrary Nix expression, not a plain value.
 * Relative path literals inside `src` resolve against the process's
 * current working directory (matching real Nix: a `--arg`/`--argstr`
 * value has no "containing file" of its own to resolve against). */
Value ll_eval_expr_source(const char *src);

/* Same pipeline as ll_eval_file, but for bootstrapping the `derivation`
 * wrapper itself (derivation.c's ll_make_derivation_wrapper_value) --
 * breaks the self-referential cycle that would otherwise result from
 * building a root Env's own `derivation` slot, see eval_file.c's
 * make_root_env_ex comment for the full rationale. Not useful for
 * anything else. */
Value ll_eval_file_for_derivation_bootstrap(const char *path);

/* The resolved absolute path of the OUTERMOST file passed to
 * ll_eval_file (i.e. argv's <file.nix>, main.c) -- captured once, the
 * first time ll_eval_file runs, and never overwritten by any nested
 * `import`/`scopedImport`. NULL if no file has been evaluated yet or if
 * the process only ever ran ll_eval_expr_source.
 *
 * This exists for exactly one caller: `builtins.unsafeGetAttrPos`
 * (builtins_m54.c). It is a deliberately coarse approximation of "which
 * file defines this attribute": real Nix tracks the position of the
 * SPECIFIC binding that built the attribute's value, which can be any
 * file an attrset literal was ever written in and then passed/merged/
 * imported across any number of boundaries; asnix's runtime Attrs
 * representation has no such back-pointer, so this reports "the file
 * asnix was originally told to evaluate" uniformly for every attribute
 * that exists, regardless of which file actually wrote it. */
const char *ll_eval_top_level_file(void);

#endif /* LL_EVAL_FILE_H */
