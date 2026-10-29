/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_BYTECODE_H
#define LL_BYTECODE_H

#include <stddef.h>
#include <stdint.h>

#include "ll/ir.h"

int ll_bytecode_serialize(IrProgram *prog, uint8_t **out_buf, size_t *out_len);

IrProgram *ll_bytecode_deserialize(const uint8_t *buf, size_t len);

#endif /* LL_BYTECODE_H */
