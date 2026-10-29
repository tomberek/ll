/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_IRPASS_JMPNOP_H
#define LL_IRPASS_JMPNOP_H

/* Registers the "jmpnop" bytecode pass -- see irpass_jmpnop.c's own
 * header comment for what it does. */
void ll_irpass_register_jmpnop(void);

#endif /* LL_IRPASS_JMPNOP_H */
