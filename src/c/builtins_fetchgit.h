/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_BUILTINS_FETCHGIT_H
#define LL_BUILTINS_FETCHGIT_H

#include <stdint.h>

#include "ll/value.h"

Value ll_prim_fetchGit(const Value *args, int64_t argc);
Value ll_make_fetchGit_primop(void);

#endif /* LL_BUILTINS_FETCHGIT_H */
