/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_FLAKE_EVAL_H
#define LL_FLAKE_EVAL_H

#include "ll/value.h"

/*
 * `./asnix --flake DIR[#attr.path]` -- the smallest useful slice of real
 * Nix flake support, scoped deliberately narrow.
 *
 * `ll_eval_flake_outputs(dir)` evaluates `dir/flake.nix`, requires it
 * to declare NO non-empty `inputs` (the only case handled: a flake that
 * is entirely self-contained, or whose `outputs` function only ever
 * reads `self`), builds `self`/`sourceInfo` for `dir` by NAR-hashing and
 * store-copying it exactly like `builtins.fetchGit`/`builtins.path`
 * already do for any other local tree, then calls `outputs` the same
 * way cppnix's `call-flake.nix` does (`outputs (inputs // { self =
 * result; })`, with `inputs = {}` since inputs are never resolved) and
 * returns the resulting `result` attrset (`_type`, `inputs`, `outputs`,
 * `outPath`, `sourceInfo`, plus every field the flake's own `outputs`
 * returned). Fatal (message to stderr, exit 1) on anything not
 * supported: `dir` is not a directory, has no `flake.nix`, or
 * `flake.nix` declares a non-empty `inputs` attribute.
 *
 * Caller (main.c) is responsible for splitting a `DIR#attr.path`
 * command-line argument into `dir` and the attrpath fragment, and for
 * applying that fragment to the returned Value (reusing the exact same
 * `-A`-style `select_attrpath` machinery already there).
 */
Value ll_eval_flake_outputs(const char *dir);

#endif /* LL_FLAKE_EVAL_H */
