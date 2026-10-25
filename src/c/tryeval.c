/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "tryeval.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "asnix/ir.h" /* ASNIX_THUNK_OFF_ENV/STATE, ASNIX_THUNK_STATE_FAILED */

static jmp_buf *g_tryeval_stack[ASNIX_TRYEVAL_MAX_DEPTH];
static int g_tryeval_depth = 0;

/* M82: stacklimits.S's intrusive IN_PROGRESS-thunk stack -- see
 * tryeval.h's own comment and ir.h's ASNIX_THUNK_OFF_ENV comment. Each
 * node's "prev" link is stashed in the thunk's own (otherwise unused
 * while IN_PROGRESS) `env` field, so this is a plain linked-list walk
 * over raw thunk-cell addresses, not a separate allocation. */
extern int64_t asnix_inprogress_head;

int64_t asnix_tryeval_save_inprogress_head(void) {
    return asnix_inprogress_head;
}

void asnix_tryeval_unwind_inprogress(int64_t saved_head) {
    int64_t cur = asnix_inprogress_head;
    while (cur != saved_head) {
        char *cell = (char *)(intptr_t)cur;
        int64_t next = *(int64_t *)(cell + ASNIX_THUNK_OFF_ENV);
        /* M103: OFF_STATE's IN_PROGRESS-time pointer bits (there are
         * none left to preserve -- the one read that mattered already
         * happened at the UNFORCED->IN_PROGRESS transition) are simply
         * overwritten with the bare FAILED tag, same as before. */
        *(int64_t *)(cell + ASNIX_THUNK_OFF_STATE) = ASNIX_THUNK_STATE_FAILED;
        cur = next;
    }
    asnix_inprogress_head = saved_head;
}

void asnix_tryeval_push(jmp_buf *buf) {
    if (g_tryeval_depth >= ASNIX_TRYEVAL_MAX_DEPTH) {
        fprintf(stderr, "asnix: tryEval nesting too deep, aborting\n");
        exit(1);
    }
    g_tryeval_stack[g_tryeval_depth++] = buf;
}

void asnix_tryeval_pop(void) {
    g_tryeval_depth--;
}

_Noreturn void asnix_tryeval_raise_or_fatal(const char *msg, size_t len) {
    if (g_tryeval_depth > 0) {
        longjmp(*g_tryeval_stack[g_tryeval_depth - 1], 1);
    }
    write(2, msg, len);
    exit(1);
}
