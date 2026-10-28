/* M-iets-parity: ported from iets's own src/sys/linux.rs (read directly,
 * not guessed) -- before ever requesting a huge page via MADV_HUGEPAGE,
 * check whether the kernel actually HAS free order-9+ (>= 2MiB) blocks
 * to satisfy it. transparent_hugepage/defrag=madvise (confirmed via
 * /sys on the dev machine) means a madvise'd region that the kernel
 * can't satisfy from an already-free block gets its huge page assembled
 * by DIRECT (synchronous) compaction in the calling thread -- iets's own
 * comment documents measuring this as a real regression on a fragmented
 * box. Checked once per process (cached), matching iets's own
 * LazyLock<bool> -- fragmentation during one process's own short
 * lifetime isn't worth re-checking.
 *
 * Called from arena.S/env_arena.S (hand-written asm, no libc dependency
 * of their own) via the C ABI -- this one helper is the sole reason
 * those files gained a non-syscall C dependency; kept to this single,
 * narrow helper rather than pulling broader C machinery into them. */
#define _DEFAULT_SOURCE /* strtok_r() -- same feature-test macro eval_file.c uses for getcwd */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Bytes free in blocks of order 9 (2^9 pages = 2MiB at a 4KiB page
 * size) and up, summed across every zone/node line in /proc/buddyinfo.
 * Mirrors iets's own huge_free(): skip the "Node N, zone NAME" prefix
 * (4 whitespace-separated tokens), then each remaining token is the
 * free-block count at increasing order, starting from 0. */
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

/* 1 GiB, matching iets's own HUGE_FREE_MIN exactly. */
#define ASNIX_HUGE_FREE_MIN (1ULL << 30)

/* -1 = not yet checked, 0 = checked, not safe, 1 = checked, safe. */
static int g_hugepage_state = -1;

int asnix_huge_pages_available(void) {
    if (g_hugepage_state < 0) {
        g_hugepage_state = (huge_free_bytes() >= ASNIX_HUGE_FREE_MIN) ? 1 : 0;
    }
    return g_hugepage_state;
}
