/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_ARENA_H
#define LL_ARENA_H

/*
 * Never-free bump arena: nothing allocated through ll_arena_alloc is ever
 * freed individually -- object layout, not allocation speed, decides peak
 * memory (same tradeoff ~/gnix/iets makes). The LL_MAX_MEMORY safety
 * valve below exists so "never free" fails cleanly (a printed message and a
 * nonzero exit) instead of being silently OOM-killed mid-build.
 *
 * Field offsets below are shared verbatim between the C struct and the
 * hand-written asm in src/asm/arena.S -- if you reorder fields here, update
 * the LL_ARENA_OFF_* constants (and arena.S's uses of them) to match.
 */

#define LL_ARENA_OFF_CUR   0  /* uint8_t  *cur:             bump pointer within the current chunk */
#define LL_ARENA_OFF_END   8  /* uint8_t  *end:             one past the end of the current chunk */
#define LL_ARENA_OFF_TOTAL 16 /* uint64_t  total_allocated: bytes actually handed to callers so far */
#define LL_ARENA_OFF_MAX   24 /* uint64_t  max_memory:      cap in bytes; 0 = unlimited */

#define LL_ARENA_CHUNK_SIZE 0x200000 /* 2 MiB default chunk size */

#ifndef __ASSEMBLER__

#include <stdint.h>

typedef struct {
    uint8_t *cur;
    uint8_t *end;
    uint64_t total_allocated;
    uint64_t max_memory;
} LlArenaState;

extern LlArenaState ll_arena;

/* Resets the arena to empty and sets the memory cap (0 = unlimited). Any
 * chunks from a previous session are simply abandoned (never-free design --
 * only meant to be called once at process start, or between test cases in
 * the test harness where leaking a previous chunk's mapping is harmless). */
void ll_arena_init(uint64_t max_memory_bytes);

/* Returns an 8-byte-aligned, zero-length-safe pointer to `n` freshly
 * allocated bytes, or aborts the process (message to stderr, exit 1) if the
 * allocation would exceed the configured memory cap or mmap fails. */
void *ll_arena_alloc(uint64_t n);

/* M-iets-parity (hugepage_check.c): 1 iff /proc/buddyinfo shows at
 * least 1GiB free in order-9+ (>=2MiB) blocks, checked once and
 * cached. arena.S/env_arena.S check this before ever hinting
 * MADV_HUGEPAGE, to avoid triggering expensive synchronous compaction
 * on a fragmented box (see that file's own comment). */
int ll_huge_pages_available(void);

#endif /* __ASSEMBLER__ */

#endif /* LL_ARENA_H */
