/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "asnix/stats.h"

#include <stdio.h>
#include <stdlib.h>

uint64_t asnix_stats_env = 0;
uint64_t asnix_stats_env_elements = 0;
uint64_t asnix_stats_thunk = 0;
uint64_t asnix_stats_closure = 0;
uint64_t asnix_stats_attrs = 0;
uint64_t asnix_stats_attrs_entries = 0;
uint64_t asnix_stats_list = 0;
uint64_t asnix_stats_list_elements = 0;
uint64_t asnix_stats_cell = 0;
uint64_t asnix_stats_lazyapp = 0;
uint64_t asnix_stats_thunkmap_desc = 0;
uint64_t asnix_stats_thunkmap_elem = 0;
uint64_t asnix_stats_string = 0;
uint64_t asnix_stats_primop = 0;
uint64_t asnix_stats_path = 0;

void asnix_stats_print(void) {
    if (!getenv("ASNIX_SHOW_STATS")) return;
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
            (unsigned long)asnix_stats_env, (unsigned long)asnix_stats_env_elements,
            (unsigned long)asnix_stats_attrs, (unsigned long)asnix_stats_attrs_entries,
            (unsigned long)asnix_stats_list, (unsigned long)asnix_stats_list_elements,
            (unsigned long)asnix_stats_thunk, (unsigned long)asnix_stats_closure,
            (unsigned long)asnix_stats_cell, (unsigned long)asnix_stats_lazyapp,
            (unsigned long)asnix_stats_thunkmap_desc, (unsigned long)asnix_stats_thunkmap_elem,
            (unsigned long)asnix_stats_string,
            (unsigned long)asnix_stats_primop, (unsigned long)asnix_stats_path);
}
