/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef ASNIX_BUILTINS_LIST2_H
#define ASNIX_BUILTINS_LIST2_H

#include <stddef.h>

#include "builtins_misc.h" /* shared AsnixBuiltinMiscDef/-Builder {name, build()} shape */

/*
 * Second round of list builtins, added alongside genericClosure
 * (builtins_closure.c): elem, any, all, concatMap, sort, genList,
 * partition, foldl, foldr, catAttrs. Kept separate from M17's
 * builtins_list.c (map/filter/foldl'/elemAt/concatLists/tail/head) rather
 * than appended to it, matching this project's existing one-feature-batch-
 * per-file convention for builtins modules.
 *
 * All of these are true arity>=2 builtins, so this module uses the
 * {name, build()} shape (builtins_misc.h) rather than plain
 * {name, fn, arity} -- see builtins_shared.h's make_curried_native, the
 * mechanism every entry's `build()` uses.
 */

extern const AsnixBuiltinMiscDef ASNIX_LIST2_BUILTINS[];
extern const size_t ASNIX_LIST2_BUILTINS_COUNT;

#include "asnix/value.h" /* Value */
#include <stdint.h>       /* int64_t */

/* M110: exported so compiler.c's try_fused_native_call2 can fuse a
 * statically-recognized, fully-saturated `builtins.<name> a b` call site
 * into one direct native call, skipping the curried-native Closure/Env
 * hops -- see compiler.c's own comment for the recognition + safety
 * conditions. `elem`'s is NOFORCE (see its own comment above); `any`/
 * `all` are ordinary force-by-default natives. */
Value asnix_prim_elem2_impl(const Value *args, int64_t argc);
Value asnix_prim_any2_impl(const Value *args, int64_t argc);
Value asnix_prim_all2_impl(const Value *args, int64_t argc);

#endif /* ASNIX_BUILTINS_LIST2_H */
