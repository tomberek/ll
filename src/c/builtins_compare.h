/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_BUILTINS_COMPARE_H
#define LL_BUILTINS_COMPARE_H

#include <stddef.h>

#include "builtins_misc.h" /* shared LlBuiltinMiscDef/-Builder {name, build()} shape */

/*
 * Compare/string-domain builtins added in this pass: lessThan,
 * splitVersion, compareVersions, replaceStrings, stringToCharacters,
 * toJSON. `splitVersion`'s component-splitting rule and
 * `compareVersions`'s component-ordering rule are ported line-for-line
 * from the real implementation (~/nix/src/libstore/names.cc's
 * nextComponent/componentsLT/compareVersions), the same "port from the
 * real source, don't guess" discipline this project already uses for
 * base32.c/store_path.c (see docs/architecture.md's M8 notes) -- these
 * two in particular have enough genuinely surprising edge cases (e.g.
 * `"1pre" < "1"`, `"1.0" > "1"`) that re-deriving them from a description
 * would be asking to get them wrong.
 */

extern const LlBuiltinMiscDef LL_COMPARE_BUILTINS[];
extern const size_t LL_COMPARE_BUILTINS_COUNT;

/* M69: the exact same recursive, compact JSON encoding `builtins.toJSON`
 * itself uses internally (`json_encode`: sorted attrset keys, nlohmann-
 * compatible float/string/UTF-8 rules) -- exposed as its own entry point
 * so derivation.c's `__structuredAttrs` support can reuse it to serialize
 * a whole attrset of input attributes (including values that aren't
 * coercible to a plain string, like a nested attrset: `env = { foo =
 * "bar"; }`) instead of writing a second JSON serializer from scratch.
 * `prim_toJSON` itself is now just this function plus the Value-wrapping
 * step. Returns a malloc'd, NUL-terminated string (caller frees). */
char *ll_toJSON_cstr(Value v);

#endif /* LL_BUILTINS_COMPARE_H */
