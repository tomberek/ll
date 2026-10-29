/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_BUILTINS_CLOSURE_H
#define LL_BUILTINS_CLOSURE_H

#include <stddef.h>

#include "ll/value.h"
#include "builtins_misc.h" /* shared LlBuiltinMiscDef/-Builder {name, build()} shape */

/*
 * builtins.genericClosure -- deliberately cut from the M17 builtins-
 * expansion pass (see builtins_list.c's old header comment and
 * docs/architecture.md's M17 section: "genericClosure deliberately cut
 * ... documented as a follow-up, not attempted"). Implemented here, in its
 * own translation unit (not folded into builtins_list.c), since it's a
 * large-enough, self-contained feature to deserve its own file rather than
 * crowding list-builtins with its BFS/dedup machinery.
 *
 * Uses the {name, build()} shape (builtins_misc.h), not the plain
 * {name, fn, arity} one (builtins_list.h) -- genericClosure is a true
 * arity-1 native function (one attrset argument, no currying needed), so
 * it would fit either shape, but every *other* module added in this same
 * pass needs the Closure-chain-building `build()` shape for its arity>=2
 * entries (see builtins_shared.h's make_curried_native) -- using it
 * uniformly here too, even though this one entry doesn't strictly need
 * it, keeps all four new modules' tables the same shape.
 */

extern const LlBuiltinMiscDef LL_CLOSURE_BUILTINS[];
extern const size_t LL_CLOSURE_BUILTINS_COUNT;

#endif /* LL_BUILTINS_CLOSURE_H */
