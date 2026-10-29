/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_ARENA_H
#define LL_ARENA_H

/*
 * Field offsets below are shared verbatim between the C struct and the
 * hand-written asm in src/asm/arena.S -- if you reorder fields here, update
 * the LL_ARENA_OFF_* constants (and arena.S's uses of them) to match.
 */

#define LL_ARENA_OFF_CUR   0  /* uint8_t  *cur:             bump pointer within the current chunk */
#define LL_ARENA_OFF_END   8  /* uint8_t  *end:             one past the end of the current chunk */
#define LL_ARENA_OFF_TOTAL 16 /* uint64_t  total_allocated: bytes actually handed to callers so far */
#define LL_ARENA_OFF_MAX   24 /* uint64_t  max_memory:      cap in bytes; 0 = unlimited */

#define LL_ARENA_CHUNK_SIZE 0x200000

#ifndef __ASSEMBLER__

#include <stdint.h>

typedef struct {
    uint8_t *cur;
    uint8_t *end;
    uint64_t total_allocated;
    uint64_t max_memory;
} LlArenaState;

extern LlArenaState ll_arena;

void ll_arena_init(uint64_t max_memory_bytes);

void *ll_arena_alloc(uint64_t n);

int ll_huge_pages_available(void);

#endif /* __ASSEMBLER__ */

#endif /* LL_ARENA_H */
