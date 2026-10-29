/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_IRPASS_DEADFRAME_H
#define LL_IRPASS_DEADFRAME_H

/* Registers the "deadframe" bytecode pass -- see irpass_deadframe.c's own
 * header comment for what it does (dead zero-slot FRAME_ALLOC/FRAME_EXIT
 * bracket elimination, with the depth-fixup it needs to stay correct
 * when the bracket's own body still reaches past it). */
void ll_irpass_register_deadframe(void);

#endif /* LL_IRPASS_DEADFRAME_H */
