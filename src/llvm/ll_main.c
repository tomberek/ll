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
#include "builtins_shared.h"
#include "compiler.h"
#include "flake_eval.h"
#include "ll_print.h"
#include "parser.h"
#include "root_env.h"
#include "symtab.h"
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

static void print_usage(FILE *out) {
    fprintf(out,
        "usage: ll [-h | --help] [-E EXPR | FILE | --flake DIR[#attr.path]] [-A attrpath]\n"
        "\n"
        "Evaluate a Nix expression and print the result.\n"
        "\n"
        "  -E, --expr EXPR   evaluate EXPR instead of FILE\n"
        "  --flake DIR[#attr.path]   evaluate DIR as a flake and print its outputs\n"
        "                            (or the attrpath fragment after '#', if given)\n"
        "  -A attrpath       select attrpath out of the evaluated result\n"
        "  -h, --help        print this message and exit\n"
        "\n"
        "With neither FILE, -E, nor --flake, evaluates the literal expression \"42\".\n");
}

/* Ported from asnix's own main.c select_attrpath -- a plain dotted-path
 * walk, one attrset lookup per segment. Used by both -A and a --flake
 * DIR#attr.path fragment. */
static Value select_attrpath(Value v, const char *attrpath) {
    const char *p = attrpath;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t seglen = dot ? (size_t)(dot - p) : strlen(p);
        v = ll_force_for_print(v);
        if (ll_tag_of(v) != LL_TAG_ATTRS) {
            fprintf(stderr, "ll: -A: '%.*s' is not a set\n", (int)seglen, p);
            exit(1);
        }
        uint32_t sym = ll_intern(p, seglen);
        int64_t idx = ll_bshared_attrs_find(v, sym);
        if (idx < 0) {
            fprintf(stderr, "ll: -A: attribute '%.*s' missing\n", (int)seglen, p);
            exit(1);
        }
        v = ll_bshared_attrs_value(v, idx);
        p = dot ? dot + 1 : p + seglen;
    }
    return ll_force_for_print(v);
}

int main(int argc, char **argv) {
    const char *expr_flag = NULL;
    const char *file_arg = NULL;
    const char *flake_ref = NULL;
    const char *attrpath = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(stdout);
            return 0;
        } else if (strcmp(argv[i], "-E") == 0 || strcmp(argv[i], "--expr") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "ll: %s requires an argument\n", argv[i]);
                print_usage(stderr);
                return 1;
            }
            expr_flag = argv[++i];
        } else if (strcmp(argv[i], "--flake") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "ll: --flake requires an argument\n");
                print_usage(stderr);
                return 1;
            }
            flake_ref = argv[++i];
        } else if (strcmp(argv[i], "-A") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "ll: -A requires an argument\n");
                print_usage(stderr);
                return 1;
            }
            attrpath = argv[++i];
        } else if (argv[i][0] == '-' && strcmp(argv[i], "-") != 0) {
            fprintf(stderr,
                    "ll: unrecognized flag '%s' (only -E/--expr, --flake, -A, and -h/--help are supported)\n",
                    argv[i]);
            print_usage(stderr);
            return 1;
        } else if (file_arg == NULL) {
            file_arg = argv[i];
        } else {
            fprintf(stderr, "ll: unexpected extra argument '%s'\n", argv[i]);
            print_usage(stderr);
            return 1;
        }
    }

    if ((expr_flag ? 1 : 0) + (file_arg ? 1 : 0) + (flake_ref ? 1 : 0) > 1) {
        fprintf(stderr, "ll: -E/--expr, --flake, and a FILE argument are mutually exclusive\n");
        print_usage(stderr);
        return 1;
    }

    if (flake_ref) {
        const char *hash = strchr(flake_ref, '#');
        char dir_buf[1024];
        const char *fragment = NULL;
        if (hash) {
            size_t dirlen = (size_t)(hash - flake_ref);
            if (dirlen >= sizeof(dir_buf)) {
                fprintf(stderr, "ll: --flake: path '%s' is too long\n", flake_ref);
                return 1;
            }
            memcpy(dir_buf, flake_ref, dirlen);
            dir_buf[dirlen] = '\0';
            fragment = hash + 1;
        } else {
            snprintf(dir_buf, sizeof(dir_buf), "%s", flake_ref);
        }
        ll_arena_init(0);
        ll_lazy_thunk_enabled = getenv("LL_EAGER_THUNK") == NULL;
        Value result = ll_eval_flake_outputs(dir_buf);
        if (fragment && fragment[0]) result = select_attrpath(result, fragment);
        if (attrpath) result = select_attrpath(result, attrpath);
        ll_print_value(result);
        return 0;
    }

    /* file_arg's own file-or-literal fallback (read_file_source returns
     * NULL for a non-existent path, so a bare literal expression
     * positional argument -- tests/difftest.sh's own convention --
     * still works unchanged); -E always evaluates its argument as a
     * literal expression, no file lookup. */
    char *file_src = file_arg ? read_file_source(file_arg) : NULL;
    const char *src = expr_flag ? expr_flag : (file_src ? file_src : (file_arg ? file_arg : "42"));
    char *base_dir, *resolved_path = NULL;
    if (file_src) {
        resolve_file_for_compile(file_arg, &base_dir, &resolved_path);
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

    if (attrpath) result = select_attrpath(result, attrpath);
    ll_print_value(result);
    return 0;
}
