/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef ASNIX_IRPASS_JMPTHREAD_H
#define ASNIX_IRPASS_JMPTHREAD_H

/* Registers the "jmpthread" bytecode pass -- see irpass_jmpthread.c's own
 * header comment for what it does. */
void asnix_irpass_register_jmpthread(void);

#endif /* ASNIX_IRPASS_JMPTHREAD_H */
