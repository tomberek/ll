/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "string_context.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static char *dup_cstr(const char *s) {
    size_t n = strlen(s);
    char *out = malloc(n + 1);
    memcpy(out, s, n + 1);
    return out;
}

void asnix_ctx_init(AsnixStringContext *ctx) {
    memset(ctx, 0, sizeof(*ctx));
}

void asnix_ctx_add_built(AsnixStringContext *ctx, const char *drv_path, const char *output_name) {
    for (size_t i = 0; i < ctx->ndrvs; i++) {
        if (strcmp(ctx->drvs[i].drv_path, drv_path) == 0 && strcmp(ctx->drvs[i].output_name, output_name) == 0)
            return;
    }
    if (ctx->ndrvs == ctx->drvs_cap) {
        ctx->drvs_cap = ctx->drvs_cap ? ctx->drvs_cap * 2 : 8;
        ctx->drvs = realloc(ctx->drvs, ctx->drvs_cap * sizeof(*ctx->drvs));
    }
    ctx->drvs[ctx->ndrvs].drv_path = dup_cstr(drv_path);
    ctx->drvs[ctx->ndrvs].output_name = dup_cstr(output_name);
    ctx->ndrvs++;
}

void asnix_ctx_add_opaque(AsnixStringContext *ctx, const char *path) {
    for (size_t i = 0; i < ctx->nsrcs; i++) {
        if (strcmp(ctx->srcs[i], path) == 0) return;
    }
    if (ctx->nsrcs == ctx->srcs_cap) {
        ctx->srcs_cap = ctx->srcs_cap ? ctx->srcs_cap * 2 : 8;
        ctx->srcs = realloc(ctx->srcs, ctx->srcs_cap * sizeof(*ctx->srcs));
    }
    ctx->srcs[ctx->nsrcs++] = dup_cstr(path);
}

/* M80: see string_context.h's own `deep` field comment. */
void asnix_ctx_add_deep(AsnixStringContext *ctx, const char *drv_path) {
    for (size_t i = 0; i < ctx->ndeep; i++) {
        if (strcmp(ctx->deep[i], drv_path) == 0) return;
    }
    if (ctx->ndeep == ctx->deep_cap) {
        ctx->deep_cap = ctx->deep_cap ? ctx->deep_cap * 2 : 8;
        ctx->deep = realloc(ctx->deep, ctx->deep_cap * sizeof(*ctx->deep));
    }
    ctx->deep[ctx->ndeep++] = dup_cstr(drv_path);
}

int asnix_ctx_is_empty(const AsnixStringContext *ctx) {
    return !ctx || (ctx->ndrvs == 0 && ctx->nsrcs == 0 && ctx->ndeep == 0);
}

void asnix_ctx_union_into(AsnixStringContext *dst, const AsnixStringContext *src) {
    if (asnix_ctx_is_empty(src)) return;
    for (size_t i = 0; i < src->ndrvs; i++) asnix_ctx_add_built(dst, src->drvs[i].drv_path, src->drvs[i].output_name);
    for (size_t i = 0; i < src->nsrcs; i++) asnix_ctx_add_opaque(dst, src->srcs[i]);
    for (size_t i = 0; i < src->ndeep; i++) asnix_ctx_add_deep(dst, src->deep[i]);
}

/* Perf note (post-M76 profiling pass): this table is NOT "small N" like
 * eval_file.c's import cache / derivation.c's masked-hash cache / nar.c's
 * copy cache -- on a real nixpkgs eval (`d.nix`,
 * `(import nixpkgs {}).hello.outPath`) it grows to ~4,500 entries, and
 * `asnix_string_context_of` is called ~34,000 times from hot string-
 * construction call sites (concat_strings_impl, asnix_coerce_to_string,
 * asnix_coerce_to_string_for_derivation). A plain linear scan here is
 * O(entries) per lookup against a table that itself grows across the
 * run, i.e. effectively O(n^2) overall -- measured at 62 MILLION scan
 * steps for that one run, ~11% of total self-time in `perf record`
 * (the single hottest leaf symbol in the whole profile). So unlike this
 * project's other side tables, this one is a measured hot path and
 * earns the hash table the "small N" convention elsewhere declines.
 *
 * Open addressing, linear probing, power-of-two capacity, no deletions
 * (nothing in this codebase ever un-attaches a context) so no tombstones
 * needed. Keyed by the Value's own (tag-masked) arena pointer, which is
 * never NULL for a real heap allocation -- NULL is used as the empty-slot
 * sentinel in `calloc`'d tables. */
typedef struct {
    const void *ptr;
    AsnixStringContext ctx;
} ContextTableEntry;
static ContextTableEntry *g_table = NULL;
static size_t g_count = 0, g_cap = 0;

static size_t ctx_hash_ptr(const void *ptr) {
    /* 64-bit mix (splitmix64's finalizer) -- arena pointers are only a
     * few bits apart from each other (bump allocation), so a naive
     * `ptr & mask` would cluster everything into a handful of buckets;
     * this spreads them across the whole table. */
    uint64_t x = (uint64_t)(uintptr_t)ptr;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return (size_t)x;
}

/* Finds the slot `ptr` belongs in within `table` (capacity `cap`, a power
 * of two): either an existing entry for `ptr`, or the first empty slot
 * probed starting from its hash -- i.e. where it already lives, or where
 * it should be inserted. Never called against a full table (the caller
 * always grows past the load-factor threshold first), so the probe is
 * guaranteed to terminate. */
static ContextTableEntry *ctx_table_find_slot(ContextTableEntry *table, size_t cap, const void *ptr) {
    size_t mask = cap - 1;
    size_t idx = ctx_hash_ptr(ptr) & mask;
    for (;;) {
        ContextTableEntry *e = &table[idx];
        if (e->ptr == NULL || e->ptr == ptr) return e;
        idx = (idx + 1) & mask;
    }
}

static void ctx_table_grow(void) {
    size_t new_cap = g_cap ? g_cap * 2 : 64;
    ContextTableEntry *new_table = calloc(new_cap, sizeof(*new_table));
    for (size_t i = 0; i < g_cap; i++) {
        if (g_table[i].ptr == NULL) continue;
        ContextTableEntry *slot = ctx_table_find_slot(new_table, new_cap, g_table[i].ptr);
        *slot = g_table[i];
    }
    free(g_table);
    g_table = new_table;
    g_cap = new_cap;
}

void asnix_string_attach_context(Value v, const AsnixStringContext *ctx) {
    if (asnix_ctx_is_empty(ctx)) return;
    /* Grow at a 75% load factor (not 100%, the old array's trigger) --
     * open addressing needs headroom or probe chains degrade toward a
     * linear scan again. */
    if (g_cap == 0 || g_count * 4 >= g_cap * 3) ctx_table_grow();
    const void *ptr = (const void *)(v.raw & ~0x7ULL);
    ContextTableEntry *entry = ctx_table_find_slot(g_table, g_cap, ptr);
    /* Same "first attach for a given pointer wins" semantics as the old
     * code: arena pointers are never reused (bump allocator, no frees),
     * so in practice this is always the new-entry branch; preserved for
     * exact parity on the off chance the same Value gets re-attached. */
    if (entry->ptr == NULL) {
        entry->ptr = ptr;
        asnix_ctx_init(&entry->ctx);
        asnix_ctx_union_into(&entry->ctx, ctx);
        g_count++;
    }
}

const AsnixStringContext *asnix_string_context_of(Value v) {
    if (g_cap == 0) return NULL;
    const void *ptr = (const void *)(v.raw & ~0x7ULL);
    ContextTableEntry *entry = ctx_table_find_slot(g_table, g_cap, ptr);
    return entry->ptr == NULL ? NULL : &entry->ctx;
}
