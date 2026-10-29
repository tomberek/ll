/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_STATS_H
#define LL_STATS_H

/* Permanent, always-on allocation counters, one per major Value-heap
 * category. Incremented unconditionally at each category's real
 * allocation site; only the PRINTING is gated behind LL_SHOW_STATS=1,
 * printed to stderr at exit from main.c. Simple standalone globals, not
 * a struct, so asm sites can `incq ll_stats_<name>(%rip)` directly. */

#ifndef __ASSEMBLER__
#include <stdint.h>

extern uint64_t ll_stats_env;
extern uint64_t ll_stats_env_elements;
extern uint64_t ll_stats_thunk;
extern uint64_t ll_stats_closure;
extern uint64_t ll_stats_attrs;
extern uint64_t ll_stats_attrs_entries;
extern uint64_t ll_stats_list;
extern uint64_t ll_stats_list_elements;
extern uint64_t ll_stats_cell;
extern uint64_t ll_stats_lazyapp;
extern uint64_t ll_stats_thunkmap_desc;  /* one per mapAttrs/zipAttrsWith CALL */
extern uint64_t ll_stats_thunkmap_elem;  /* one per output ELEMENT (no heap object of its own --
                                              * packed into the output attrset's own entry slot,
                                              * see heap.h's LL_THUNKMAP_MARKER comment) */
extern uint64_t ll_stats_string;
extern uint64_t ll_stats_primop;
extern uint64_t ll_stats_path;

/* Prints every counter to stderr. Only called from main.c, only when
 * LL_SHOW_STATS=1 is set. */
void ll_stats_print(void);
#endif /* __ASSEMBLER__ */

#endif /* LL_STATS_H */
