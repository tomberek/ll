/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "ll/irpass.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define LL_IRPASS_MAX_PASSES 32
static const char *g_names[LL_IRPASS_MAX_PASSES];
static LlIrPass g_passes[LL_IRPASS_MAX_PASSES];
static uint64_t g_fire_counts[LL_IRPASS_MAX_PASSES];
static size_t g_npasses = 0;

static size_t g_current_pass = (size_t)-1;

/* Bounds ll_irpass_remove's firing-log shift-fixup so a later program's
 * own removal never touches an earlier, already-finished program's
 * entries -- each program's ops array independently starts its own
 * indexing at 0, so op-index ranges across programs can coincide. */
static size_t g_program_firing_base = 0;

#define LL_IRPASS_MAX_PROGRAM_RANGES 65536
typedef struct {
    const void *prog;
    size_t start, end;
} LlIrProgramRange;
static LlIrProgramRange g_program_ranges[LL_IRPASS_MAX_PROGRAM_RANGES];
static size_t g_nprogram_ranges = 0;

void ll_irpass_register(const char *name, LlIrPass pass) {
    if (g_npasses >= LL_IRPASS_MAX_PASSES) {
        fprintf(stderr, "ll_irpass_register: too many passes registered\n");
        exit(1);
    }
    g_names[g_npasses] = name;
    g_passes[g_npasses] = pass;
    g_fire_counts[g_npasses] = 0;
    g_npasses++;
}

void ll_irpass_clear(void) {
    g_npasses = 0;
}

size_t ll_irpass_num_passes(void) {
    return g_npasses;
}

const char *ll_irpass_name(size_t index) {
    return g_names[index];
}

uint64_t ll_irpass_fire_count(size_t index) {
    return g_fire_counts[index];
}

#define LL_IRPASS_MAX_FIRINGS 4096
typedef struct {
    size_t pass_index;
    size_t op_index;
} LlIrFiring;
static LlIrFiring g_firings[LL_IRPASS_MAX_FIRINGS];
static size_t g_nfirings = 0;

#ifdef LL_IRDUMP_LINE_TRACKING
static uint32_t *g_tracked_lines = NULL;

void ll_irpass_set_line_array(uint32_t *lines) {
    g_tracked_lines = lines;
}
#endif

void ll_irpass_reset_stats(void) {
    for (size_t i = 0; i < g_npasses; i++) g_fire_counts[i] = 0;
    g_nfirings = 0;
    g_nprogram_ranges = 0;
    g_program_firing_base = 0;
}

void ll_irpass_print_stats(FILE *out) {
    if (g_npasses == 0) return;
    int any = 0;
    for (size_t i = 0; i < g_npasses; i++) {
        if (g_fire_counts[i] == 0) continue;
        fprintf(out, "%s: %lu firings\n", g_names[i], (unsigned long)g_fire_counts[i]);
        any = 1;
    }
    if (!any) fprintf(out, "(no passes fired)\n");
}

size_t ll_irpass_num_firings(void) {
    return g_nfirings;
}

const char *ll_irpass_firing_pass_name(size_t i) {
    return g_names[g_firings[i].pass_index];
}

size_t ll_irpass_firing_pass_index(size_t i) {
    return g_firings[i].pass_index;
}

size_t ll_irpass_firing_op_index(size_t i) {
    return g_firings[i].op_index;
}

void ll_irpass_begin_program(void) {
    g_program_firing_base = g_nfirings;
}

void ll_irpass_mark_program(const void *prog) {
    if (g_npasses == 0) return;
    if (g_nfirings == g_program_firing_base) return;
    if (g_nprogram_ranges >= LL_IRPASS_MAX_PROGRAM_RANGES) return;
    g_program_ranges[g_nprogram_ranges].prog = prog;
    g_program_ranges[g_nprogram_ranges].start = g_program_firing_base;
    g_program_ranges[g_nprogram_ranges].end = g_nfirings;
    g_nprogram_ranges++;
}

int ll_irpass_firing_belongs_to_program(size_t i, const void *prog) {
    for (size_t r = 0; r < g_nprogram_ranges; r++) {
        if (g_program_ranges[r].prog == prog && i >= g_program_ranges[r].start && i < g_program_ranges[r].end)
            return 1;
    }
    return 0;
}

/* Which op-ids carry a byte-delta-to-target in field `a`. Exhaustive:
 * nothing else in OP_COUNT's enum uses a self-relative index delta. */
static int op_is_jump(int op_id) {
    return op_id == OP_IF || op_id == OP_JMP || op_id == OP_WITH_TRY || op_id == OP_SELECT_OR ||
           op_id == OP_SELECT_DYNAMIC_OR || op_id == OP_EQ_IF || op_id == OP_NEQ_IF || op_id == OP_LT_IF ||
           op_id == OP_GE_IF || op_id == OP_HAS_ATTR_IF;
}

void ll_irpass_remove(IrOp *ops, size_t *len, size_t start, size_t count) {
    size_t old_len = *len;
    if (count == 0) return;
    if (start + count > old_len) {
        fprintf(stderr, "ll_irpass_remove: range [%zu,%zu) out of bounds for len=%zu\n", start, start + count,
                old_len);
        exit(1);
    }
    for (size_t i = 0; i < old_len; i++) {
        int id = (int)(intptr_t)ops[i].handler;
        if (!op_is_jump(id)) continue;
        int64_t target = (int64_t)i + ops[i].a / LL_IROP_SIZE;
        if (target >= (int64_t)start && target < (int64_t)(start + count)) {
            fprintf(stderr,
                    "ll_irpass_remove: op %zu jumps into the removed range [%zu,%zu) -- "
                    "a pass may only remove self-contained, unreferenced sequences\n",
                    i, start, start + count);
            exit(1);
        }
    }
    memmove(&ops[start], &ops[start + count], (old_len - start - count) * sizeof(IrOp));
#ifdef LL_IRDUMP_LINE_TRACKING
    if (g_tracked_lines) {
        memmove(&g_tracked_lines[start], &g_tracked_lines[start + count],
                (old_len - start - count) * sizeof(uint32_t));
    }
#endif
    size_t new_len = old_len - count;
    for (size_t i = 0; i < new_len; i++) {
        int id = (int)(intptr_t)ops[i].handler;
        if (!op_is_jump(id)) continue;
        size_t old_i = (i < start) ? i : i + count;
        int64_t old_target = (int64_t)old_i + ops[i].a / LL_IROP_SIZE;
        int64_t new_target = (old_target < (int64_t)start) ? old_target : old_target - (int64_t)count;
        ops[i].a = (new_target - (int64_t)i) * LL_IROP_SIZE;
    }
    /* Bounded to >= g_program_firing_base: an earlier program's own
     * already-finished entries must never be touched here. */
    for (size_t i = g_program_firing_base; i < g_nfirings; i++) {
        size_t idx = g_firings[i].op_index;
        if (idx >= start + count) g_firings[i].op_index = idx - count;
        else if (idx >= start) g_firings[i].op_index = start > 0 ? start - 1 : 0;
    }
    if (g_current_pass != (size_t)-1 && g_nfirings < LL_IRPASS_MAX_FIRINGS) {
        g_firings[g_nfirings].pass_index = g_current_pass;
        g_firings[g_nfirings].op_index = start > 0 ? start - 1 : 0;
        g_nfirings++;
    }
    *len = new_len;
}

int ll_irpass_run(IrOp *ops, size_t *len, int64_t **pool, size_t *pool_len, size_t *pool_cap) {
    int any_changed_ever = 0;
    for (int iter = 0; iter < LL_IRPASS_MAX_ITERS; iter++) {
        int changed = 0;
        for (size_t p = 0; p < g_npasses; p++) {
            g_current_pass = p;
            int fired = g_passes[p](ops, len, pool, pool_len, pool_cap);
            g_current_pass = (size_t)-1;
            if (fired) {
                g_fire_counts[p]++;
                changed = 1;
            }
        }
        if (!changed) break;
        any_changed_ever = 1;
    }
    return any_changed_ever;
}

size_t ll_irpass_pool_append(int64_t **pool, size_t *pool_len, size_t *pool_cap, int64_t v) {
    if (*pool_len == *pool_cap) {
        *pool_cap = *pool_cap ? *pool_cap * 2 : 8;
        *pool = realloc(*pool, *pool_cap * sizeof(int64_t));
    }
    (*pool)[*pool_len] = v;
    return (*pool_len)++;
}
