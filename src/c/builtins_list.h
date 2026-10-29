/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_BUILTINS_LIST_H
#define LL_BUILTINS_LIST_H

#include <stddef.h>
#include <stdint.h>

#include "ll/value.h"

/*
 * builtins-list stage: list-processing builtins (map, filter, foldl',
 * elemAt, concatLists, tail, head) implemented in builtins_list.c, kept in
 * their own file/table (not builtins.c's BUILTINS[] directly) so this
 * agent and the sibling agents also adding builtins in parallel don't
 * textually collide on the same array. A later integration step merges
 * ll_builtins_list[] into the shared BUILTINS[] table in builtins.c
 * (same {name, fn, arity} shape as that file's local BuiltinDef, so the
 * merge is a straight concatenation, not a rewrite).
 *
 * genericClosure is NOT included here -- see builtins_list.c's header
 * comment for why it was cut rather than rushed.
 */

typedef Value (*LlPrimFn)(const Value *args, int64_t argc);

typedef struct {
    const char *name;
    LlPrimFn fn;
    int64_t arity;
} LlBuiltinDef;

extern const LlBuiltinDef ll_builtins_list[];
extern const size_t ll_builtins_list_count;

/* `map` is one of a small, fixed set of names real Nix exposes as a bare
 * global in addition to `builtins.map` (confirmed directly against
 * nix-instantiate -- see builtins.h's ll_make_import_primop comment
 * for the full list checked). Wraps the exact same native function
 * `builtins.map` uses. */
Value ll_make_map_primop(void);

#endif /* LL_BUILTINS_LIST_H */
