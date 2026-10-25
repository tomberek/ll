/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef ASNIX_IRPASS_STRCONCAT_H
#define ASNIX_IRPASS_STRCONCAT_H

/* Registers the "strconcat" bytecode pass -- see irpass_strconcat.c's own
 * header comment for what it does. */
void asnix_irpass_register_strconcat(void);

#endif /* ASNIX_IRPASS_STRCONCAT_H */
