/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_BUILTINS_M23_H
#define LL_BUILTINS_M23_H

#include <stddef.h>
#include <stdint.h>

#include "builtins_misc.h"

extern const LlBuiltinMiscDef LL_M23_BUILTINS[];
extern const size_t LL_M23_BUILTINS_COUNT;

Value ll_prim_pathExists(const Value *args, int64_t argc);
Value ll_prim_readFile(const Value *args, int64_t argc);
Value ll_prim_readFileType(const Value *args, int64_t argc);
Value ll_prim_readDir(const Value *args, int64_t argc);
Value ll_prim_toPath(const Value *args, int64_t argc);
Value ll_prim_getEnv(const Value *args, int64_t argc);
Value ll_prim_parseDrvName(const Value *args, int64_t argc);

Value ll_prim_intersectAttrs2_impl(const Value *args, int64_t argc);

#endif /* LL_BUILTINS_M23_H */
