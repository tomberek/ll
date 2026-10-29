/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_BUILTINS_ATTRS_H
#define LL_BUILTINS_ATTRS_H

#include <stddef.h>
#include <stdint.h>

#include "ll/value.h"
#include "builtins_list.h" /* shared LlPrimFn/LlBuiltinDef typedefs */

/*
 * builtins-attrs task: attrNames/attrValues/hasAttr/getAttr/mapAttrs/
 * removeAttrs, in their own translation unit (src/c/builtins_attrs.c) to
 * avoid merge conflicts with sibling agents also adding builtins -- see
 * src/c/builtins.c's BUILTINS[]/BuiltinDef for the shape this mirrors.
 *
 * Integration step: concatenate LL_ATTRS_BUILTINS into the table
 * ll_make_builtins_attrs() (builtins.c) walks, the same way it already
 * walks its own BUILTINS[]. LlPrimFn/LlBuiltinDef themselves come
 * from builtins_list.h (two sibling agents independently defined the
 * identical shape; integration kept one copy rather than two).
 */

extern const LlBuiltinDef LL_ATTRS_BUILTINS[];
extern const size_t LL_ATTRS_BUILTINS_COUNT;

/* Exposes `removeAttrs` as a bare root-scope global (see eval_file.c's
 * root Env / compiler.c's root ScopeFrame) -- one of real Nix's small set
 * of bare globals (M18's import/map/abort/toString, extended here). */
Value ll_make_removeAttrs_primop(void);

#endif /* LL_BUILTINS_ATTRS_H */
