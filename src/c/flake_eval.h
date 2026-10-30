/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_FLAKE_EVAL_H
#define LL_FLAKE_EVAL_H

#include "ll/value.h"

/*
 * `./ll --flake DIR[#attr.path]` -- real flake support via the real,
 * vendored `call-flake.nix` (src/c/call-flake.nix), not a hand-rolled
 * approximation: declared inputs are genuinely resolved through an
 * actual `flake.lock` (or a synthesized trivial one, for a flake that
 * declares none), recursively `import`ing each input's own
 * `flake.nix` and threading it through the same `self`/`result`
 * fixed point real Nix uses.
 *
 * `ll_eval_flake_outputs(dir)` resolves `dir` to an absolute path,
 * reads `dir/flake.lock` if present (or synthesizes
 * `{"version":7,"root":"root","nodes":{"root":{}}}` if absent),
 * builds the root's own `sourceInfo` directly from `dir` (NAR-hashed
 * for `narHash`, but `outPath` is `dir` itself -- never store-copied,
 * matching real Nix's own `overrides` escape hatch for "a previously
 * fetched tree already on disk"), and calls the vendored
 * `call-flake.nix` with that lock string, `{ root = { sourceInfo =
 * ...; }; }` as `overrides`, and a real fetchTree dispatcher
 * (`ll_prim_fetch_tree_final`, this file) as `fetchTreeFinal`. Only
 * `path` (absolute) and `git` locked-input types are implemented --
 * an honest, narrow slice; any other `type` fails loudly, not
 * silently.
 *
 * Caller (main.c) is responsible for splitting a `DIR#attr.path`
 * command-line argument into `dir` and the attrpath fragment, and for
 * applying that fragment to the returned Value.
 */
Value ll_eval_flake_outputs(const char *dir);

#endif /* LL_FLAKE_EVAL_H */
