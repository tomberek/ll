/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef ASNIX_IRPASS_NOTFOLD_H
#define ASNIX_IRPASS_NOTFOLD_H

/* Registers the boolean-NOT constant-folding pass: folds
 * `OP_PUSH_TRUE; OP_NOT` -> `OP_PUSH_FALSE` and
 * `OP_PUSH_FALSE; OP_NOT` -> `OP_PUSH_TRUE` (2 ops down to 1) -- see
 * irpass_notfold.c for why this is always safe, unlike a hypothetical
 * general NOT;NOT elision over an arbitrary value. */
void asnix_irpass_register_notfold(void);

#endif /* ASNIX_IRPASS_NOTFOLD_H */
