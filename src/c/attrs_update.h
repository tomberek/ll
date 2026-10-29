/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_ATTRS_UPDATE_H
#define LL_ATTRS_UPDATE_H

#include "ll/value.h"

Value ll_attrs_update(Value lhs, Value rhs);

Value ll_attrs_eager_merge(Value lhs, Value rhs);

Value ll_attrs_insert_dynamic(Value attrs, Value name_val, Value value);

#endif /* LL_ATTRS_UPDATE_H */
