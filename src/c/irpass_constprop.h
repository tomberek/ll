/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef ASNIX_IRPASS_CONSTPROP_H
#define ASNIX_IRPASS_CONSTPROP_H

/* Registers the frame-slot constant-propagation pass: when a `let`-style
 * binding's slot is set to a bare compile-time constant (PUSH_INT/
 * PUSH_TRUE/PUSH_FALSE/PUSH_NULL) and never reassigned, every later
 * same-scope OP_VAR_REF read of that slot is rewritten to the same push
 * directly, skipping the frame-slot read entirely -- see
 * irpass_constprop.c for the full safety argument. */
void asnix_irpass_register_constprop(void);

#endif /* ASNIX_IRPASS_CONSTPROP_H */
