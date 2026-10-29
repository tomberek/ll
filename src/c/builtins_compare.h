/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_BUILTINS_COMPARE_H
#define LL_BUILTINS_COMPARE_H

#include <stddef.h>

#include "builtins_misc.h"

extern const LlBuiltinMiscDef LL_COMPARE_BUILTINS[];
extern const size_t LL_COMPARE_BUILTINS_COUNT;

/* Returns a malloc'd, NUL-terminated string (caller frees). */
char *ll_toJSON_cstr(Value v);

#endif /* LL_BUILTINS_COMPARE_H */
