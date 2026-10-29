/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_BUILTINS_M54_H
#define LL_BUILTINS_M54_H

#include <stddef.h>

#include "ll/value.h"

typedef Value (*LlBuiltinM54Builder)(void);

typedef struct {
    const char *name;
    LlBuiltinM54Builder build;
} LlBuiltinM54Def;

extern const LlBuiltinM54Def LL_M54_BUILTINS[];
extern const size_t LL_M54_BUILTINS_COUNT;

Value ll_make_scopedImport_primop(void);

#endif /* LL_BUILTINS_M54_H */
