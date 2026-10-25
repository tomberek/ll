/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef ASNIX_IRPASS_CMPNOT_H
#define ASNIX_IRPASS_CMPNOT_H

/* Registers the "cmpnot" bytecode pass -- see irpass_cmpnot.c's own header
 * comment for what it does (and what it deliberately does NOT do, and
 * why). */
void asnix_irpass_register_cmpnot(void);

#endif /* ASNIX_IRPASS_CMPNOT_H */
