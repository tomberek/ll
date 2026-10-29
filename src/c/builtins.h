/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_BUILTINS_H
#define LL_BUILTINS_H

#include "ll/value.h"

/* Builds the `builtins` attrset -- sorted by symbol id, matching
 * OP_SELECT's expectations. */
Value ll_make_builtins_attrs(void);

Value ll_make_builtins_attrs_for_derivation_bootstrap(void);

Value ll_make_import_primop(void);

Value ll_make_abort_primop(void);

Value ll_make_throw_primop(void);

Value ll_make_toString_primop(void);

#endif /* LL_BUILTINS_H */
