/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_BUILTINS_LIST2_H
#define LL_BUILTINS_LIST2_H

#include <stddef.h>

#include "builtins_misc.h"

extern const LlBuiltinMiscDef LL_LIST2_BUILTINS[];
extern const size_t LL_LIST2_BUILTINS_COUNT;

#include "ll/value.h"
#include <stdint.h>

Value ll_prim_elem2_impl(const Value *args, int64_t argc);
Value ll_prim_any2_impl(const Value *args, int64_t argc);
Value ll_prim_all2_impl(const Value *args, int64_t argc);

#endif /* LL_BUILTINS_LIST2_H */
