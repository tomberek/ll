/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_BUILTINS_JSON_H
#define LL_BUILTINS_JSON_H

#include <stddef.h>
#include <stdint.h>

#include "builtins_misc.h"

extern const LlBuiltinMiscDef LL_JSON_BUILTINS[];
extern const size_t LL_JSON_BUILTINS_COUNT;

Value ll_json_parse_structured_attrs(const char *bytes, int64_t len);

#endif /* LL_BUILTINS_JSON_H */
