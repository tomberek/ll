/* M10: end-to-end CLI: lexer -> parser -> compiler (src/c, vendored
 * from asnix) -> ir_link (dispatch_table.c) -> asnix_eval (ll_eval.c's
 * AOT threaded dispatch over src/llvm/ops/*.c's handlers), with
 * `builtins`/`map`/`import`/etc. bound via root_env.c's
 * ll_make_root_env. No LLVM involved at runtime at all -- see
 * ~/.claude/plans/soft-wishing-engelbart.md's M10 section for why: the
 * op handlers are plain C, compiled once ahead of time, same as
 * asnix's own hand-written asm handlers are assembled once into
 * asnix's own binary.
 *
 * Printing uses ll_print.c, a verbatim port of asnix's own main.c
 * printer (quoted strings, attrsets sorted by key, <PRIMOP>/<LAMBDA>
 * states, cycle detection) -- real Nix values, not just integers. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

#include "asnix/arena.h"
#include "asnix/ir.h"
#include "asnix/value.h"
#include "ast.h"
#include "compiler.h"
#include "ll_print.h"
#include "parser.h"
#include "root_env.h"

static double ms_between(struct timespec *a, struct timespec *b) {
    return (b->tv_sec - a->tv_sec) * 1000.0 + (b->tv_nsec - a->tv_nsec) / 1e6;
}

/* If argv[1] names an existing, readable regular file, read it as the
 * source -- matching asnix's own CLI convention (a file path, not an
 * inline expression), so a file-based workload can be benchmarked
 * against asnix without an extra shell/`cat` process to turn a file
 * into a giant command-line argument first (that extra process was
 * being measured as if it were this evaluator's own cost -- see
 * bench/results/M8-findings.md's M10 section). Falls back to treating
 * argv[1] as a literal expression otherwise, preserving
 * tests/difftest.sh's existing convention (none of its 44 fixed
 * expressions are themselves valid file paths). */
static char *read_file_source(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = malloc((size_t)st.st_size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

int main(int argc, char **argv) {
    char *file_src = argc > 1 ? read_file_source(argv[1]) : NULL;
    const char *src = file_src ? file_src : (argc > 1 ? argv[1] : "42");
    const char *show_stats = getenv("LL_SHOW_STATS");
    struct timespec t0, t1, t2, t3, t4;

    if (show_stats) clock_gettime(CLOCK_MONOTONIC, &t0);
    asnix_arena_init(0);
    Ast *root = asnix_parse(src);
    if (show_stats) clock_gettime(CLOCK_MONOTONIC, &t1);
    IrProgram *prog = asnix_compile(root, "/tmp", NULL);
    if (show_stats) clock_gettime(CLOCK_MONOTONIC, &t2);
    Env *root_env = ll_make_root_env();
    if (show_stats) clock_gettime(CLOCK_MONOTONIC, &t3);

    Value result = asnix_eval(prog, root_env);
    if (show_stats) clock_gettime(CLOCK_MONOTONIC, &t4);

    if (show_stats) {
        fprintf(stderr, "ll stats: parse=%.3fms compile=%.3fms root_env=%.3fms eval=%.3fms total=%.3fms\n",
                ms_between(&t0, &t1), ms_between(&t1, &t2), ms_between(&t2, &t3), ms_between(&t3, &t4),
                ms_between(&t0, &t4));
    }

    ll_print_value(result);
    return 0;
}
