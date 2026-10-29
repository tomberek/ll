/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_BUILTINS_MISC_H
#define LL_BUILTINS_MISC_H

#include <stddef.h>

#include "ll/value.h"

/* "Misc" builtins (string ops + type predicates), kept in their own
 * translation unit so this agent's work doesn't textually collide with
 * sibling agents also extending builtins.c's BUILTINS[] table -- a later
 * integration step merges LL_BUILTINS_MISC into the shared table (see
 * builtins.c's ll_make_builtins_attrs).
 *
 * Each entry's `build` is called once, AFTER ll_arena_init() has run
 * (mirrors ll_make_builtins_attrs's own lazy-build-at-use pattern in
 * builtins.c), and returns the fully-tagged Value to insert into the
 * `builtins` attrset under `name`. Most entries build a plain arity-N
 * PrimOp the usual way; the two genuinely multi-argument ones
 * (`substring`, `concatStringsSep`) build a short chain of real Closures
 * instead of a PrimOp -- see the long comment above make_curried_native()
 * in builtins_misc.c for why a PrimOp-only curry (hashString's
 * finite-enum trick) can't generalize to arbitrary-domain arguments like
 * an integer start/length or an arbitrary separator string. */
typedef Value (*LlBuiltinMiscBuilder)(void);

typedef struct {
    const char *name;
    LlBuiltinMiscBuilder build;
} LlBuiltinMiscDef;

extern const LlBuiltinMiscDef LL_BUILTINS_MISC[];
extern const size_t LL_BUILTINS_MISC_COUNT;

/* Exposes `isNull` as a bare root-scope global (see eval_file.c's root
 * Env / compiler.c's root ScopeFrame) -- one of real Nix's small set of
 * bare globals (M18's import/map/abort/toString, extended here). */
Value ll_make_isNull_primop(void);

/* Same idea, for `baseNameOf`/`dirOf` (M23) -- also real Nix bare
 * globals (see builtins.h's ll_make_import_primop comment for the
 * full curated list). */
Value ll_make_baseNameOf_primop(void);
Value ll_make_dirOf_primop(void);
Value ll_make_placeholder_primop(void);

#endif /* LL_BUILTINS_MISC_H */
