/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_BUILTINS_TOML_H
#define LL_BUILTINS_TOML_H

#include <stddef.h>

#include "ll/value.h"
#include "builtins_misc.h"

extern const LlBuiltinMiscDef LL_TOML_BUILTINS[];
extern const size_t LL_TOML_BUILTINS_COUNT;

Value ll_make_fromTOML_primop(void);

#endif /* LL_BUILTINS_TOML_H */
