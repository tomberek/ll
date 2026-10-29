/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "ll/stats.h"

#include <stdio.h>
#include <stdlib.h>

uint64_t ll_stats_env = 0;
uint64_t ll_stats_env_elements = 0;
uint64_t ll_stats_thunk = 0;
uint64_t ll_stats_closure = 0;
uint64_t ll_stats_attrs = 0;
uint64_t ll_stats_attrs_entries = 0;
uint64_t ll_stats_list = 0;
uint64_t ll_stats_list_elements = 0;
uint64_t ll_stats_cell = 0;
uint64_t ll_stats_lazyapp = 0;
uint64_t ll_stats_thunkmap_desc = 0;
uint64_t ll_stats_thunkmap_elem = 0;
uint64_t ll_stats_string = 0;
uint64_t ll_stats_primop = 0;
uint64_t ll_stats_path = 0;

void ll_stats_print(void) {
    if (!getenv("LL_SHOW_STATS")) return;
    fprintf(stderr,
            "asnix stats:\n"
            "  envs:     %lu (%lu elements)\n"
            "  sets:     %lu (%lu entries)\n"
            "  list:     %lu (%lu elements)\n"
            "  thunks:   %lu\n"
            "  closures: %lu\n"
            "  cells:    %lu\n"
            "  lazyapps: %lu\n"
            "  thunkmap: %lu descs, %lu elements\n"
            "  strings:  %lu\n"
            "  primops:  %lu\n"
            "  paths:    %lu\n",
            (unsigned long)ll_stats_env, (unsigned long)ll_stats_env_elements,
            (unsigned long)ll_stats_attrs, (unsigned long)ll_stats_attrs_entries,
            (unsigned long)ll_stats_list, (unsigned long)ll_stats_list_elements,
            (unsigned long)ll_stats_thunk, (unsigned long)ll_stats_closure,
            (unsigned long)ll_stats_cell, (unsigned long)ll_stats_lazyapp,
            (unsigned long)ll_stats_thunkmap_desc, (unsigned long)ll_stats_thunkmap_elem,
            (unsigned long)ll_stats_string,
            (unsigned long)ll_stats_primop, (unsigned long)ll_stats_path);
}
