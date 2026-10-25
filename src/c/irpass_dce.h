/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef ASNIX_IRPASS_DCE_H
#define ASNIX_IRPASS_DCE_H

/* Registers the "dce" bytecode pass -- see irpass_dce.c's own header
 * comment for what it does (global reachability-based dead code
 * elimination) and why it finds 0 opportunities on anything today's
 * compiler emits. */
void asnix_irpass_register_dce(void);

#endif /* ASNIX_IRPASS_DCE_H */
