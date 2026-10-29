/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "tryeval.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "ll/ir.h" /* LL_THUNK_OFF_ENV/STATE, LL_THUNK_STATE_FAILED */

static jmp_buf *g_tryeval_stack[LL_TRYEVAL_MAX_DEPTH];
static int g_tryeval_depth = 0;

/* stacklimits.S's intrusive IN_PROGRESS-thunk stack -- see tryeval.h's
 * own comment and ir.h's LL_THUNK_OFF_ENV comment. Each
 * node's "prev" link is stashed in the thunk's own (otherwise unused
 * while IN_PROGRESS) `env` field, so this is a plain linked-list walk
 * over raw thunk-cell addresses, not a separate allocation. */
extern int64_t ll_inprogress_head;

int64_t ll_tryeval_save_inprogress_head(void) {
    return ll_inprogress_head;
}

void ll_tryeval_unwind_inprogress(int64_t saved_head) {
    int64_t cur = ll_inprogress_head;
    while (cur != saved_head) {
        char *cell = (char *)(intptr_t)cur;
        int64_t next = *(int64_t *)(cell + LL_THUNK_OFF_ENV);
        /* OFF_STATE's IN_PROGRESS-time pointer bits (there are none
         * left to preserve -- the one read that mattered already
         * happened at the UNFORCED->IN_PROGRESS transition) are simply
         * overwritten with the bare FAILED tag. */
        *(int64_t *)(cell + LL_THUNK_OFF_STATE) = LL_THUNK_STATE_FAILED;
        cur = next;
    }
    ll_inprogress_head = saved_head;
}

void ll_tryeval_push(jmp_buf *buf) {
    if (g_tryeval_depth >= LL_TRYEVAL_MAX_DEPTH) {
        fprintf(stderr, "ll: tryEval nesting too deep, aborting\n");
        exit(1);
    }
    g_tryeval_stack[g_tryeval_depth++] = buf;
}

void ll_tryeval_pop(void) {
    g_tryeval_depth--;
}

_Noreturn void ll_tryeval_raise_or_fatal(const char *msg, size_t len) {
    if (g_tryeval_depth > 0) {
        longjmp(*g_tryeval_stack[g_tryeval_depth - 1], 1);
    }
    write(2, msg, len);
    exit(1);
}
