/* End-to-end CLI: lexer -> parser -> compiler (src/c, vendored from
 * asnix) -> ir_link (dispatch_table.c) -> ll_eval (ll_eval.c's AOT
 * threaded dispatch over src/llvm/ops/*.c's handlers), with
 * `builtins`/`map`/`import`/etc. bound via root_env.c's
 * ll_make_root_env. No LLVM involved at runtime at all.
 *
 * Printing uses ll_print.c, a verbatim port of asnix's own main.c
 * printer (quoted strings, attrsets sorted by key, <PRIMOP>/<LAMBDA>
 * states, cycle detection) -- real Nix values, not just integers. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ll/arena.h"
#include "ll/ir.h"
#include "ll/value.h"
#include "ast.h"
#include "compiler.h"
#include "ll_print.h"
#include "parser.h"
#include "root_env.h"
#include "value_util.h" /* ll_canon_path */

static double ms_between(struct timespec *a, struct timespec *b) {
    return (b->tv_sec - a->tv_sec) * 1000.0 + (b->tv_nsec - a->tv_nsec) / 1e6;
}

/* If argv[1] names an existing, readable regular file, read it as the
 * source -- matching asnix's own CLI convention (a file path, not an
 * inline expression). Falls back to treating argv[1] as a literal
 * expression otherwise, preserving tests/difftest.sh's existing
 * convention (none of its fixed expressions are themselves valid file
 * paths). */
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

static char *current_dir(void) {
    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd)) {
        fprintf(stderr, "ll: cannot determine the current directory\n");
        exit(1);
    }
    return strdup(cwd);
}

/* Matches asnix's own eval_file.c exactly: resolve_import_path (make
 * `path` absolute against the process cwd if it's relative, then
 * lexically canonicalize) followed by dir_of (directory part of an
 * absolute path -- "/foo.nix" -> "/", the one case that keeps its own
 * slash). Every relative `import`/path-literal in the evaluated file
 * resolves against *base_dir_out, so a file argument NOT sitting
 * directly in the process's own cwd needs its real directory here, not
 * a placeholder. *resolved_path_out is the canonicalized absolute path
 * itself, read by __curPos__/builtins.unsafeGetAttrPos's own `file`
 * field -- a NULL here prints that field empty instead of the real
 * path. */
static void resolve_file_for_compile(const char *path, char **base_dir_out, char **resolved_path_out) {
    char *abs;
    if (path[0] == '/') {
        abs = strdup(path);
    } else {
        char *cwd = current_dir();
        size_t cwd_len = strlen(cwd), path_len = strlen(path);
        abs = malloc(cwd_len + 1 + path_len + 1);
        memcpy(abs, cwd, cwd_len);
        abs[cwd_len] = '/';
        memcpy(abs + cwd_len + 1, path, path_len + 1);
        free(cwd);
    }
    char *canon = ll_canon_path(abs);
    free(abs);

    const char *slash = strrchr(canon, '/');
    size_t dirlen = (size_t)(slash - canon);
    if (dirlen == 0) dirlen = 1;
    char *dir = malloc(dirlen + 1);
    memcpy(dir, canon, dirlen);
    dir[dirlen] = '\0';

    *base_dir_out = dir;
    *resolved_path_out = canon;
}

int main(int argc, char **argv) {
    char *file_src = argc > 1 ? read_file_source(argv[1]) : NULL;
    const char *src = file_src ? file_src : (argc > 1 ? argv[1] : "42");
    char *base_dir, *resolved_path = NULL;
    if (file_src) {
        resolve_file_for_compile(argv[1], &base_dir, &resolved_path);
    } else {
        base_dir = current_dir();
    }
    const char *show_stats = getenv("LL_SHOW_STATS");
    struct timespec t0, t1, t2, t3, t4;

    if (show_stats) clock_gettime(CLOCK_MONOTONIC, &t0);
    ll_arena_init(0);
    /* Lazy-thunk-compile experiment, ported from asnix: default ON here
     * (unlike asnix, which keeps it opt-in). LL_EAGER_THUNK=1 is the
     * rollback switch if a regression ever surfaces. */
    ll_lazy_thunk_enabled = getenv("LL_EAGER_THUNK") == NULL;
    Ast *root = ll_parse(src);
    if (show_stats) clock_gettime(CLOCK_MONOTONIC, &t1);
    IrProgram *prog = ll_compile(root, base_dir, resolved_path);
    if (show_stats) clock_gettime(CLOCK_MONOTONIC, &t2);
    Env *root_env = ll_make_root_env();
    if (show_stats) clock_gettime(CLOCK_MONOTONIC, &t3);

    Value result = ll_eval(prog, root_env);
    if (show_stats) clock_gettime(CLOCK_MONOTONIC, &t4);

    if (show_stats) {
        fprintf(stderr, "ll stats: parse=%.3fms compile=%.3fms root_env=%.3fms eval=%.3fms total=%.3fms\n",
                ms_between(&t0, &t1), ms_between(&t1, &t2), ms_between(&t2, &t3), ms_between(&t3, &t4),
                ms_between(&t0, &t4));
    }

    ll_print_value(result);
    return 0;
}
