/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_IRPASS_CFOLD_H
#define LL_IRPASS_CFOLD_H

/* Registers this framework's one shipped example/proof-of-concept pass:
 * folds `OP_PUSH_INT a; OP_PUSH_INT b; OP_<binop>` into a single
 * OP_PUSH_INT/OP_PUSH_TRUE/OP_PUSH_FALSE for every pure int-int binary op
 * this IR has (ADD/SUB/MUL/DIV and the 6 comparisons) -- see
 * irpass_cfold.c. Exists mainly to exercise ll_irpass_remove's
 * splice-and-refixup machinery end to end with a real, if modest, win. */
void ll_irpass_register_cfold(void);

#endif /* LL_IRPASS_CFOLD_H */
