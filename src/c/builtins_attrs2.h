/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef ASNIX_BUILTINS_ATTRS2_H
#define ASNIX_BUILTINS_ATTRS2_H

#include <stddef.h>

#include "builtins_misc.h" /* shared AsnixBuiltinMiscDef/-Builder {name, build()} shape */

/*
 * Second round of attrset builtins, added alongside genericClosure
 * (builtins_closure.c) and builtins_list2.c: filterAttrs, listToAttrs,
 * attrByPath. Kept separate from M17's builtins_attrs.c (attrNames/
 * attrValues/hasAttr/getAttr/mapAttrs/removeAttrs).
 *
 * `attrByPath` isn't actually a core Nix builtin (confirmed:
 * `builtins ? attrByPath` is `false` on real Nix) -- it's `lib.attrByPath`.
 * Added under `builtins` anyway, matching `builtins.foldl`/
 * `builtins.stringToCharacters` elsewhere in this pass, since this
 * project has no "with lib" root scope for a user program to reach a
 * `lib`-namespaced version instead.
 */

extern const AsnixBuiltinMiscDef ASNIX_ATTRS2_BUILTINS[];
extern const size_t ASNIX_ATTRS2_BUILTINS_COUNT;

#endif /* ASNIX_BUILTINS_ATTRS2_H */
