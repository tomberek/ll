/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_BUILTINS_FETCHGIT_H
#define LL_BUILTINS_FETCHGIT_H

#include <stdint.h>

#include "ll/value.h"

/*
 * M82: `builtins.fetchGit` -- see builtins_fetchgit.c's own header comment
 * for the full picture (scope, what's real vs. documented-cut, and the
 * verification this was checked against).
 *
 * Exposed both ways real Nix exposes it: `builtins.fetchGit` (registered
 * in builtins.c's plain BUILTINS[] table, same shape every other
 * {name,fn,arity} entry there uses) and the bare top-level global
 * `fetchGit` (ll_make_fetchGit_primop, wired into eval_file.c's
 * make_root_env_ex / compiler.c's init_builtins_frame alongside
 * `import`/`throw`/`derivation`/etc -- every primop real Nix registers
 * without a `__` prefix gets this treatment, see builtins.h's own
 * comment on that rule). Both wrap the exact same native function, same
 * precedent as `ll_make_import_primop`/`ll_make_throw_primop`.
 */
Value ll_prim_fetchGit(const Value *args, int64_t argc);
Value ll_make_fetchGit_primop(void);

#endif /* LL_BUILTINS_FETCHGIT_H */
