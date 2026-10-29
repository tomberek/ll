/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
/* Per-depth scratch-buffer cache backing src/asm/force.S's
 * ll_force_for_print -- see include/ll/force_scratch.h's header
 * comment for why this exists and why indexing by depth (rather than a
 * single cached slot) is both correct and necessary in practice. */
#include "ll/force_scratch.h"

#include <stdlib.h>

#include "ll/arena.h"

typedef struct {
    void *valstack;
    void *retstack;
} ForceScratchSlot;

static ForceScratchSlot *g_slots = NULL;
static size_t g_cap = 0;

/* Grows to at least `depth + 1` slots, zeroing the newly added ones so
 * a not-yet-cached slot's pointers read back as NULL. */
static void ensure_cap(size_t depth) {
    if (depth < g_cap) return;
    size_t new_cap = g_cap ? g_cap * 2 : 8;
    while (new_cap <= depth) new_cap *= 2;
    g_slots = realloc(g_slots, new_cap * sizeof(ForceScratchSlot));
    for (size_t i = g_cap; i < new_cap; i++) {
        g_slots[i].valstack = NULL;
        g_slots[i].retstack = NULL;
    }
    g_cap = new_cap;
}

void *ll_force_scratch_valstack(int64_t depth) {
    ensure_cap((size_t)depth);
    ForceScratchSlot *slot = &g_slots[depth];
    if (!slot->valstack) slot->valstack = ll_arena_alloc(LL_FORCE_VALSTACK_BYTES);
    return slot->valstack;
}

void *ll_force_scratch_retstack(int64_t depth) {
    ensure_cap((size_t)depth);
    ForceScratchSlot *slot = &g_slots[depth];
    if (!slot->retstack) slot->retstack = ll_arena_alloc(LL_FORCE_RETSTACK_BYTES);
    return slot->retstack;
}

/* Independent per-depth cache for src/asm/apply.S's ll_apply -- see
 * force_scratch.h's own comment on ll_apply_scratch_valstack for why
 * this exists and why it's a second, separate cache rather than reusing
 * g_slots above. */
static ForceScratchSlot *g_apply_slots = NULL;
static size_t g_apply_cap = 0;

static void apply_ensure_cap(size_t depth) {
    if (depth < g_apply_cap) return;
    size_t new_cap = g_apply_cap ? g_apply_cap * 2 : 8;
    while (new_cap <= depth) new_cap *= 2;
    g_apply_slots = realloc(g_apply_slots, new_cap * sizeof(ForceScratchSlot));
    for (size_t i = g_apply_cap; i < new_cap; i++) {
        g_apply_slots[i].valstack = NULL;
        g_apply_slots[i].retstack = NULL;
    }
    g_apply_cap = new_cap;
}

void *ll_apply_scratch_valstack(int64_t depth) {
    apply_ensure_cap((size_t)depth);
    ForceScratchSlot *slot = &g_apply_slots[depth];
    if (!slot->valstack) slot->valstack = ll_arena_alloc(LL_FORCE_VALSTACK_BYTES);
    return slot->valstack;
}

void *ll_apply_scratch_retstack(int64_t depth) {
    apply_ensure_cap((size_t)depth);
    ForceScratchSlot *slot = &g_apply_slots[depth];
    if (!slot->retstack) slot->retstack = ll_arena_alloc(LL_FORCE_RETSTACK_BYTES);
    return slot->retstack;
}

/* Independent per-depth cache for src/asm/dispatch.S's ll_eval -- see
 * force_scratch.h's own comment on ll_eval_scratch_valstack for why. */
static ForceScratchSlot *g_eval_slots = NULL;
static size_t g_eval_cap = 0;

static void eval_ensure_cap(size_t depth) {
    if (depth < g_eval_cap) return;
    size_t new_cap = g_eval_cap ? g_eval_cap * 2 : 8;
    while (new_cap <= depth) new_cap *= 2;
    g_eval_slots = realloc(g_eval_slots, new_cap * sizeof(ForceScratchSlot));
    for (size_t i = g_eval_cap; i < new_cap; i++) {
        g_eval_slots[i].valstack = NULL;
        g_eval_slots[i].retstack = NULL;
    }
    g_eval_cap = new_cap;
}

void *ll_eval_scratch_valstack(int64_t depth) {
    eval_ensure_cap((size_t)depth);
    ForceScratchSlot *slot = &g_eval_slots[depth];
    if (!slot->valstack) slot->valstack = ll_arena_alloc(LL_EVAL_VALSTACK_BYTES);
    return slot->valstack;
}

void *ll_eval_scratch_retstack(int64_t depth) {
    eval_ensure_cap((size_t)depth);
    ForceScratchSlot *slot = &g_eval_slots[depth];
    if (!slot->retstack) slot->retstack = ll_arena_alloc(LL_EVAL_RETSTACK_BYTES);
    return slot->retstack;
}
