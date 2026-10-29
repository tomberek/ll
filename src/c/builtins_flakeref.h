/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_BUILTINS_FLAKEREF_H
#define LL_BUILTINS_FLAKEREF_H

#include <stddef.h>

#include "ll/value.h"

/*
 * M66: `builtins.parseFlakeRef` / `builtins.flakeRefToString` -- pure
 * string<->attrset parsing/formatting of the flake-ref URL-like syntax
 * (`github:owner/repo/ref?dir=...`, `git+https://...`, `path:/...`, bare
 * flake ids like `nixpkgs`, tarball/file URLs, ...). No filesystem/network
 * access, no actual flake resolution/fetching/registry lookup -- see
 * docs/architecture.md's M66 scope notes for the full spec derivation
 * (reverse-engineered from numtide/nix-conformance's
 * eval-okay-*flake-ref*.nix/.exp fixtures, cross-checked directly against
 * real `nix-instantiate --eval` for every ambiguous case) and the list of
 * deliberate scope cuts. Kept in its own translation unit, same
 * "no textual collision with a sibling agent's own builtins module"
 * rationale as every prior builtins-expansion pass (see builtins_m54.h).
 */
typedef Value (*LlBuiltinFlakerefBuilder)(void);

typedef struct {
    const char *name;
    LlBuiltinFlakerefBuilder build;
} LlBuiltinFlakerefDef;

extern const LlBuiltinFlakerefDef LL_FLAKEREF_BUILTINS[];
extern const size_t LL_FLAKEREF_BUILTINS_COUNT;

#endif /* LL_BUILTINS_FLAKEREF_H */
