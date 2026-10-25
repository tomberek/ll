/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef ASNIX_STATS_H
#define ASNIX_STATS_H

/* Permanent, always-on allocation counters, one per major Value-heap
 * category -- a standing `NIX_SHOW_STATS=1`-equivalent for this project.
 * Incremented unconditionally at each category's real allocation site
 * (a plain global `incq`/`++` is cheap enough to always pay; only the
 * PRINTING is gated behind ASNIX_SHOW_STATS=1, printed to stderr at exit
 * from main.c). Names match the categories this session's own memory
 * investigations (docs/memory-accounting-asnix-vs-nix.md) already
 * measured, so a dump here is directly comparable to those reports
 * without translation. Simple standalone globals, not a struct -- same
 * convention as this project's existing asnix_apply_depth/asnix_eval_depth
 * counters, so asm sites can `incq asnix_stats_<name>(%rip)` directly. */

#ifndef __ASSEMBLER__
#include <stdint.h>

extern uint64_t asnix_stats_env;
extern uint64_t asnix_stats_env_elements;
extern uint64_t asnix_stats_thunk;
extern uint64_t asnix_stats_closure;
extern uint64_t asnix_stats_attrs;
extern uint64_t asnix_stats_attrs_entries;
extern uint64_t asnix_stats_list;
extern uint64_t asnix_stats_list_elements;
extern uint64_t asnix_stats_cell;
extern uint64_t asnix_stats_lazyapp;
extern uint64_t asnix_stats_thunkmap_desc;  /* M118: one per mapAttrs/zipAttrsWith CALL */
extern uint64_t asnix_stats_thunkmap_elem;  /* M118: one per output ELEMENT (no heap object of its
                                              * own -- packed straight into the output attrset's
                                              * own entry slot, see heap.h's ASNIX_THUNKMAP_MARKER
                                              * comment) */
extern uint64_t asnix_stats_string;
extern uint64_t asnix_stats_primop;
extern uint64_t asnix_stats_path;

/* Prints every counter to stderr, same style as M98's
 * ASNIX_BYTECODE_CACHE_STATS -- only called from main.c, only when
 * ASNIX_SHOW_STATS=1 is set. */
void asnix_stats_print(void);
#endif /* __ASSEMBLER__ */

#endif /* ASNIX_STATS_H */
