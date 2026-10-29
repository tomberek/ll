/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_BUILTINS_MISC_H
#define LL_BUILTINS_MISC_H

#include <stddef.h>

#include "ll/value.h"

typedef Value (*LlBuiltinMiscBuilder)(void);

typedef struct {
    const char *name;
    LlBuiltinMiscBuilder build;
} LlBuiltinMiscDef;

extern const LlBuiltinMiscDef LL_BUILTINS_MISC[];
extern const size_t LL_BUILTINS_MISC_COUNT;

Value ll_make_isNull_primop(void);

Value ll_make_baseNameOf_primop(void);
Value ll_make_dirOf_primop(void);
Value ll_make_placeholder_primop(void);

#endif /* LL_BUILTINS_MISC_H */
