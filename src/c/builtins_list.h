/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_BUILTINS_LIST_H
#define LL_BUILTINS_LIST_H

#include <stddef.h>
#include <stdint.h>

#include "ll/value.h"

typedef Value (*LlPrimFn)(const Value *args, int64_t argc);

typedef struct {
    const char *name;
    LlPrimFn fn;
    int64_t arity;
} LlBuiltinDef;

extern const LlBuiltinDef ll_builtins_list[];
extern const size_t ll_builtins_list_count;

Value ll_make_map_primop(void);

#endif /* LL_BUILTINS_LIST_H */
