/* Before ever requesting a huge page via MADV_HUGEPAGE, check whether
 * the kernel actually HAS free order-9+ (>= 2MiB) blocks to satisfy it.
 * With transparent_hugepage/defrag=madvise, a madvise'd region that the
 * kernel can't satisfy from an already-free block gets its huge page
 * assembled by DIRECT (synchronous) compaction in the calling thread,
 * which is a real stall risk on a fragmented box. Checked once per
 * process (cached) -- fragmentation during one process's own short
 * lifetime isn't worth re-checking.
 *
 * Called from arena.S/env_arena.S (hand-written asm, no libc dependency
 * of their own) via the C ABI -- this one helper is the sole reason
 * those files gained a non-syscall C dependency. */
#define _DEFAULT_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Bytes free in blocks of order 9 (2^9 pages = 2MiB at a 4KiB page
 * size) and up, summed across every zone/node line in /proc/buddyinfo.
 * Skip the "Node N, zone NAME" prefix (4 whitespace-separated tokens),
 * then each remaining token is the free-block count at increasing
 * order, starting from 0. */
static uint64_t huge_free_bytes(void) {
    FILE *f = fopen("/proc/buddyinfo", "r");
    if (!f) return 0;

    uint64_t total = 0;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *tok = line;
        char *saveptr;
        tok = strtok_r(line, " \t\n", &saveptr);
        int field = 0;
        int order = 0;
        while (tok) {
            if (field >= 4) {
                if (order >= 9) {
                    uint64_t count = 0;
                    for (const char *p = tok; *p; p++) {
                        if (*p < '0' || *p > '9') { count = 0; break; }
                        count = count * 10 + (uint64_t)(*p - '0');
                    }
                    /* bytes = count * page_size(4096) * 2^order */
                    total += count << (12 + order);
                }
                order++;
            }
            field++;
            tok = strtok_r(NULL, " \t\n", &saveptr);
        }
    }
    fclose(f);
    return total;
}

/* 1 GiB. */
#define LL_HUGE_FREE_MIN (1ULL << 30)

/* -1 = not yet checked, 0 = checked, not safe, 1 = checked, safe. */
static int g_hugepage_state = -1;

int ll_huge_pages_available(void) {
    if (g_hugepage_state < 0) {
        g_hugepage_state = (huge_free_bytes() >= LL_HUGE_FREE_MIN) ? 1 : 0;
    }
    return g_hugepage_state;
}
