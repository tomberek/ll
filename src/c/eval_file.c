/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
/* getcwd() is POSIX, not C11 -- needs an explicit feature-test macro
 * under -std=c11 (see Makefile's CFLAGS). */
#define _DEFAULT_SOURCE

#include "eval_file.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ll/env.h"
#include "ll/ir.h"
#include "builtins.h"
#include "builtins_attrs.h"
#include "builtins_fetchgit.h"
#include "builtins_list.h"
#include "builtins_m54.h"
#include "builtins_misc.h"
#include "builtins_shared.h"
#include "builtins_toml.h"
#include "bytecode_cache.h"
#include "compiler.h"
#include "derivation.h"
#include "lexer.h"
#include "parser.h"
#include "symtab.h"
#include "value_util.h" /* ll_canon_path, M57h symlink fix */

/* `out_len` (M92: bytecode-cache key hashing needs the exact byte count,
 * not a strlen() guess that would silently misbehave on a file
 * containing an embedded NUL) may be NULL for callers that don't. */
static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "ll: cannot open '%s'\n", path);
        exit(1);
    }
    /* M167: one fstat(fileno(f), ...) right after fopen replaces the
     * classic fseek(SEEK_END)+ftell+fseek(SEEK_SET) idiom -- glibc
     * implements that idiom via fstat+lseek+lseek under the hood (confirmed
     * via strace: 2 fstat + 2 lseek per call, one of the two fstats coming
     * from fseek(SEEK_END) itself, the other from fopen()'s own internal
     * buffering-mode setup -- that second one is unavoidable with a
     * fopen()-based implementation, so this collapses the total from 4
     * syscalls to 2, not to 1; see docs/architecture.md's M167 section for
     * the full measurement). No seek-back to SEEK_SET is needed either:
     * the file position is already 0 right after fopen, since nothing has
     * read from `f` yet.
     *
     * This does NOT get the old code's directory protection for free --
     * fstat on a directory fd SUCCEEDS (unlike ftell, which the old code
     * relied on returning -1 for a directory stream) and just reports
     * S_ISDIR in st_mode, so the explicit S_ISREG check below is
     * required, not optional. Confirmed empirically on this exact
     * filesystem (a differential standalone-harness test, not shipped)
     * that the OLD ftell-based check is actually unsound here anyway:
     * `lseek(fd, 0, SEEK_END)` on a directory fd returns LONG_MAX on
     * this kernel/fs, not -1 (`strace` confirmed it directly), so the
     * old `if (size < 0)` guard never even fired for a real directory
     * path on this machine -- the old code instead ran
     * `malloc((size_t)LONG_MAX + 1)` (fails, returns NULL) followed by
     * `fread(NULL, ...)`, which segfaults. The new S_ISREG check below
     * doesn't depend on any such filesystem-specific lseek/ftell
     * behavior, so it's strictly more robust, not just faster. */
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode)) {
        fprintf(stderr, "ll: cannot read '%s' (not a regular file?)\n", path);
        exit(1);
    }
    size_t size = (size_t)st.st_size;
    char *buf = malloc(size + 1);
    size_t n = fread(buf, 1, size, f);
    buf[n] = '\0';
    fclose(f);
    if (out_len) *out_len = n;
    return buf;
}

/* Directory part of an absolute path (as produced by resolve_import_path,
 * so always starts with '/' and has no trailing slash itself). "/foo.nix"
 * -> "/" (the one case where the result legitimately keeps its slash). */
static char *dir_of(const char *abs_path) {
    const char *slash = strrchr(abs_path, '/');
    size_t dirlen = (size_t)(slash - abs_path);
    if (dirlen == 0) dirlen = 1;
    char *dir = malloc(dirlen + 1);
    memcpy(dir, abs_path, dirlen);
    dir[dirlen] = '\0';
    return dir;
}

/* M57h: makes `path` absolute (prepending the cwd if it's relative) and
 * lexically canonicalizes it (`.`/`..`/doubled slashes -- ll_canon_path,
 * value_util.c), WITHOUT resolving symlinks on disk the way `realpath()`
 * does. This matters for relative imports reached *through* a symlinked
 * directory: `eval-okay-symlink-resolution.nix`'s fixture tree has
 * `symlink-resolution/foo/overlays -> ../overlays` (a symlink), and
 * `symlink-resolution/foo/overlays/overlay.nix` (the real file, reached
 * via that symlink) does `import ../lib`. Real Nix resolves that `..`
 * against the path AS WRITTEN/imported (`.../foo/overlays/overlay.nix`'s
 * own directory, `.../foo/overlays`) landing on `.../foo/lib` (which
 * exists) -- confirmed directly against `nix-instantiate --eval` on this
 * exact fixture tree. The previous `realpath()`-based approach instead
 * fully resolved the `foo/overlays` symlink FIRST, landing base_dir on
 * the symlink's physical TARGET directory (`symlink-resolution/overlays`,
 * since `overlays -> ../overlays`), so the same `../lib` instead resolved
 * to `symlink-resolution/lib`, which doesn't exist -- "ll: cannot
 * resolve path" (actually a `is_directory`/`read_file` failure one level
 * down, since nothing here needs `realpath` to still successfully open
 * the file: `fopen`/`stat` already follow symlinks transparently at the
 * OS level regardless of whether *we* fully resolved the path ourselves). */
static char *resolve_import_path(const char *path) {
    char *abs;
    if (path[0] == '/') {
        abs = malloc(strlen(path) + 1);
        memcpy(abs, path, strlen(path) + 1);
    } else {
        char cwd[4096];
        if (!getcwd(cwd, sizeof cwd)) {
            fprintf(stderr, "ll: cannot determine the current directory\n");
            exit(1);
        }
        size_t cwd_len = strlen(cwd), path_len = strlen(path);
        abs = malloc(cwd_len + 1 + path_len + 1);
        memcpy(abs, cwd, cwd_len);
        abs[cwd_len] = '/';
        memcpy(abs + cwd_len + 1, path, path_len + 1);
    }
    char *canon = ll_canon_path(abs);
    free(abs);
    return canon;
}

/* M82: real Nix's `resolveExprPath` (eval.cc) -- NOT to be confused with
 * M57h's `resolve_import_path` just above, which deliberately does NOT
 * resolve symlinks (needed so a relative import reached *through* a
 * symlinked DIRECTORY component resolves its own `../foo` against the
 * path as written, not the symlink's physical target -- see that
 * function's own comment). This is a narrower, different rule: real Nix
 * additionally follows a symlink chain on the exact literal path given to
 * `import` itself (the "last component", i.e. the whole path string
 * resolveExprPath was handed, as opposed to any symlinked directory
 * earlier in that path) -- `eval-okay-import-follows-symlink.nix`'s own
 * comment names the cppnix function directly. Confirmed against cppnix's
 * `resolveExprPath`: `while (lstat(path) is a symlink) path =
 * absPath(readlink(path), dirOf(path))`, i.e. the loop re-examines
 * whatever the symlink's target resolves to (itself possibly another
 * symlink, possibly reached via a *different* directory than the
 * original), which is exactly what a `link-imp.nix -> dir/imp.nix`
 * single-hop, or `link2.nix -> link-imp.nix -> dir/imp.nix` double-hop,
 * needs. Crucially, lstat()/readlink() only ever inspect the FINAL path
 * component (the OS itself transparently resolves any symlinked
 * directories earlier in the path when lstat() walks it) -- so a regular
 * file reached through a symlinked parent directory (M57h's own
 * `eval-okay-symlink-resolution.nix` fixture: `foo/overlays -> ../overlays`
 * is the symlink, but `overlay.nix` itself is a plain file) is NOT a
 * symlink itself, this loop's first lstat() sees S_ISLNK false
 * immediately, and `path` comes back completely unchanged -- so this is
 * additive, not a conflict with M57h's fix. `*out_st` reports the FINAL
 * (post-symlink-chain) path's own stat info (mode -- so the caller can
 * tell if it's a directory -- and, since M102/Finding 1, mtime/size too):
 * resolve_file_path needs this anyway and re-stat'ing would be
 * redundant; M102 additionally threads mtime/size all the way out to
 * the bytecode cache's cheap freshness check, reusing this EXACT
 * syscall's result with zero extra stat calls in the common direct-
 * file-import case (see resolve_file_path's own comment for the one
 * case -- `import <directory>` -- that still needs one more). */
static char *follow_symlink_chain(char *canon, struct stat *out_st) {
    for (int hops = 0; hops < 1024; hops++) {
        if (lstat(canon, out_st) != 0) {
            /* Doesn't exist (or unreadable) -- let the ordinary
             * read_file()/is_directory() error paths downstream report
             * this the same way they always have; nothing more to
             * resolve. */
            memset(out_st, 0, sizeof *out_st);
            return canon;
        }
        if (!S_ISLNK(out_st->st_mode)) {
            return canon;
        }
        char target[4096];
        ssize_t n = readlink(canon, target, sizeof(target) - 1);
        if (n < 0) {
            memset(out_st, 0, sizeof *out_st);
            return canon;
        }
        target[n] = '\0';
        char *dir = dir_of(canon);
        char *joined;
        if (target[0] == '/') {
            joined = malloc((size_t)n + 1);
            memcpy(joined, target, (size_t)n + 1);
        } else {
            size_t dir_len = strlen(dir);
            joined = malloc(dir_len + 1 + (size_t)n + 1);
            memcpy(joined, dir, dir_len);
            joined[dir_len] = '/';
            memcpy(joined + dir_len + 1, target, (size_t)n + 1);
        }
        free(dir);
        free(canon);
        canon = ll_canon_path(joined);
        free(joined);
    }
    fprintf(stderr, "ll: too many levels of symbolic links in path '%s'\n", canon);
    exit(1);
}

/* Resolves `path` to its final absolute form -- following the `import
 * somedir` => `somedir/default.nix` rule (verified against
 * `nix-instantiate --eval` -- found evaluating real nixpkgs,
 * `pkgs/top-level/impure.nix` does `import ./.`, i.e. imports its own
 * containing directory; previously unhandled: this directory path
 * reached read_file() directly, which "succeeded" opening it via fopen
 * but then corrupted the heap computing its size, see read_file's own
 * hardening above -- a real, serious memory-safety bug, not a clean
 * error) and M57h's symlink-preserving resolution above. Split out from
 * parse_file below (M65) so ll_eval_file can check the import cache
 * BEFORE paying for a read+lex+parse+compile+eval it might not need.
 *
 * `*out_st` (M102/Finding 1): the FINAL path's own stat info -- for the
 * common direct-file-import case this is simply follow_symlink_chain's
 * own last lstat() result, already paid for, no extra syscall. The
 * `import <directory>` case is different: follow_symlink_chain's lstat
 * describes the DIRECTORY, not the `default.nix` file actually read
 * below it, so one more `stat()` (not `lstat()` -- matches what
 * `fopen`/`fread` would transparently follow anyway) would be needed to
 * get a correct mtime/size for the file whose bytes the bytecode
 * cache's freshness check is actually about -- EXCEPT this one extra
 * syscall is only ever useful to the bytecode cache itself, so it's
 * gated behind `ll_bytecode_cache_enabled()`: with the cache off
 * (the default), `*out_st` is simply left as the directory's own stat
 * in this branch (never consulted by anything when the cache is
 * disabled) and this function costs exactly the same syscalls it
 * always did pre-M102. Found the hard way via `strace -c`: an earlier
 * version of this fix did the extra `stat()` unconditionally, which
 * silently violated this whole milestone's own "default/cache-disabled
 * behavior must be provably unaffected" constraint (confirmed: it
 * measurably added ~640 extra `newfstatat` calls to the disabled
 * baseline on ffmpeg's real closure before this guard was added). */
static char *resolve_file_path(const char *path, struct stat *out_st) {
    char *resolved = resolve_import_path(path);
    resolved = follow_symlink_chain(resolved, out_st);
    if (S_ISDIR(out_st->st_mode)) {
        size_t len = strlen(resolved);
        char *with_default = malloc(len + strlen("/default.nix") + 1);
        memcpy(with_default, resolved, len);
        memcpy(with_default + len, "/default.nix", strlen("/default.nix") + 1);
        free(resolved);
        resolved = with_default;
        if (ll_bytecode_cache_enabled()) {
            if (stat(resolved, out_st) != 0) memset(out_st, 0, sizeof *out_st);
        }
    }
    return resolved;
}

/* Reads+lexes+parses an ALREADY-RESOLVED path, writing its containing
 * directory out through `*base_dir_out` (caller-owned, malloc'd -- freed
 * once done with it, which ll_compile/ll_compile_scoped only read
 * during the one compile call), and the source-text buffer itself out
 * through `*src_out` (perf: this function's own AST nodes point directly
 * INTO src -- AstBinding.name/AstFormal.name are not copies -- so src
 * must stay alive through the caller's own subsequent
 * ll_compile()/ll_compile_scoped() call; this function can't free
 * it itself, only hand it back so the caller can free it once that
 * compile call returns, same as ll_eval_file's own free(src)). */
static Ast *parse_resolved_file(const char *resolved, char **base_dir_out, char **src_out) {
    *base_dir_out = dir_of(resolved);
    char *src = read_file(resolved, NULL);
    *src_out = src;
    Ast *ast = ll_parse(src);
    return ast;
}

/* Shared by ll_eval_file_scoped (M54) and the pre-M65 ll_eval_file
 * shape -- resolves and parses in one step, also handing the resolved
 * path back to the caller. ll_eval_file itself (below) no longer uses
 * this directly (it needs the resolved path BEFORE deciding whether to
 * parse at all, for the import cache), but ll_eval_file_scoped still
 * does, since scopedImport results are intentionally never cached (each
 * call's `scope` argument can differ, so the same file path can
 * legitimately produce a different result per call -- caching by path
 * alone would be wrong there). */
static Ast *parse_file(const char *path, char **resolved_out, char **base_dir_out, char **src_out) {
    struct stat st; /* unused here -- scopedImport never consults the bytecode
                      * cache (see this function's own comment above) */
    char *resolved = resolve_file_path(path, &st);
    *resolved_out = resolved;
    return parse_resolved_file(resolved, base_dir_out, src_out);
}

/* M65: real Nix memoizes `import <path>` by the resolved path -- a
 * second `import` of the same file returns the exact same (possibly
 * still-lazy) Value, not a fresh independent evaluation, so the file's
 * own side effects (`builtins.trace`) and work happen at most once no
 * matter how many places import it. asnix had no such cache at all,
 * confirmed directly: `import X; import X;` (X containing `builtins.trace
 * "evaluated!" 1`) printed the trace line TWICE here vs. real Nix's ONCE.
 * Found chasing d.nix's own severe slowdown: nixpkgs' bootstrap chain
 * re-imports the same handful of files (`<nix/fetchurl.nix>`,
 * `lib/default.nix`, individual `pkgs/by-name` package.nix files
 * reached through more than one alias/overlay path) over and over,
 * each time paying a full independent read+lex+parse+compile+eval --
 * confirmed via `strace -e trace=newfstatat`, which showed the exact
 * same handful of paths being stat'd tens of thousands of times (NOT an
 * infinite loop -- the process still terminates, in ~1.5s without
 * strace's own massive per-syscall overhead -- just a severe, self-
 * inflicted amount of genuinely redundant work).
 *
 * Was a plain linear-scan array, not a hash table: the original judgment
 * ("the number of distinct files a single process imports is small
 * relative to the cost of each entry, so a linear strcmp scan's cost is
 * negligible") is now confirmed stale at real nixpkgs scale -- measured
 * directly (`perf record -g` on a real `d.nix`/ffmpeg eval): this
 * function's `strcmp` calls are the single largest contributor to
 * `__strcmp_avx2` being 12.44% of ALL process self-time, because a real
 * eval imports well over a thousand distinct files (M123's own corpus:
 * 1057) and calls this on every one of the many thousands of `import`
 * *expressions* that reference them (not just once per distinct file --
 * every repeated `import <nix/fetchurl.nix>`-style reference pays a
 * full linear scan against however many distinct files have been
 * imported so far). Converted to the same open-addressed, linear-probe
 * hash-index technique `parser.c`'s `BTVec`/`btvec_index_rehash` and
 * `symtab.c`'s own table already use: a parallel `size_t` slot table
 * mapping each path's hash to its index in `g_import_cache`, grown/
 * rehashed at a 75% load factor BEFORE each insert (so rehash only ever
 * sees already-inserted entries), `g_import_cache` itself kept exactly
 * as it was (still the single source of truth for path/Value pairs;
 * the index is purely an O(1)-average lookup accelerator on top of it,
 * same relationship `BTVec.index` has to `BTVec.items`). Grows by
 * doubling, same convention as every other dynamic array in this
 * codebase. */
typedef struct {
    char *path;
    Value value;
} ImportCacheEntry;
static ImportCacheEntry *g_import_cache = NULL;
static size_t g_import_cache_count = 0, g_import_cache_cap = 0;
static size_t *g_import_cache_index = NULL;
static size_t g_import_cache_index_cap = 0;
#define IMPORT_CACHE_EMPTY_SLOT ((size_t)-1)

static uint64_t import_cache_hash(const char *s, size_t len) {
    uint64_t h = 0;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)(unsigned char)s[i];
        h *= 0x517cc1b727220a95ULL;
    }
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebULL;
    h ^= h >> 31;
    return h;
}

/* Rebuilds the index from scratch against `g_import_cache[0..
 * g_import_cache_count)` -- the entry about to be inserted is NOT yet
 * in `g_import_cache` when this is called from `import_cache_insert`
 * (matching `btvec_index_rehash`'s own documented ordering). */
static void import_cache_index_rehash(size_t new_cap) {
    size_t *new_table = malloc(new_cap * sizeof(size_t));
    for (size_t i = 0; i < new_cap; i++) new_table[i] = IMPORT_CACHE_EMPTY_SLOT;
    for (size_t i = 0; i < g_import_cache_count; i++) {
        size_t len = strlen(g_import_cache[i].path);
        size_t slot = import_cache_hash(g_import_cache[i].path, len) & (new_cap - 1);
        while (new_table[slot] != IMPORT_CACHE_EMPTY_SLOT) slot = (slot + 1) & (new_cap - 1);
        new_table[slot] = i;
    }
    free(g_import_cache_index);
    g_import_cache_index = new_table;
    g_import_cache_index_cap = new_cap;
}

static int import_cache_find(const char *resolved, Value *out) {
    if (g_import_cache_index_cap == 0) return 0;
    size_t len = strlen(resolved);
    size_t slot = import_cache_hash(resolved, len) & (g_import_cache_index_cap - 1);
    for (;;) {
        size_t idx = g_import_cache_index[slot];
        if (idx == IMPORT_CACHE_EMPTY_SLOT) return 0;
        if (strcmp(g_import_cache[idx].path, resolved) == 0) {
            *out = g_import_cache[idx].value;
            return 1;
        }
        slot = (slot + 1) & (g_import_cache_index_cap - 1);
    }
}

static void import_cache_insert(const char *resolved, Value v) {
    if (g_import_cache_count == g_import_cache_cap) {
        g_import_cache_cap = g_import_cache_cap ? g_import_cache_cap * 2 : 64;
        g_import_cache = realloc(g_import_cache, g_import_cache_cap * sizeof(ImportCacheEntry));
    }
    /* Grow/rehash the index BEFORE appending, same ordering as
     * `btvec_push` -- rehash only ever sees entries already in
     * `g_import_cache`, so the explicit insert below never collides
     * with work rehash just did. 75% load factor, matching symtab.c/
     * derivation.c/parser.c's own tables. */
    if (g_import_cache_index_cap == 0) import_cache_index_rehash(128);
    else if ((g_import_cache_count + 1) * 4 > g_import_cache_index_cap * 3) import_cache_index_rehash(g_import_cache_index_cap * 2);
    size_t len = strlen(resolved);
    char *copy = malloc(len + 1);
    memcpy(copy, resolved, len + 1);
    g_import_cache[g_import_cache_count].path = copy;
    g_import_cache[g_import_cache_count].value = v;
    size_t slot = import_cache_hash(copy, len) & (g_import_cache_index_cap - 1);
    while (g_import_cache_index[slot] != IMPORT_CACHE_EMPTY_SLOT) slot = (slot + 1) & (g_import_cache_index_cap - 1);
    g_import_cache_index[slot] = g_import_cache_count;
    g_import_cache_count++;
}

/* Fresh root env/builtins every call -- an imported file does not inherit
 * the importing file's local scope (see eval_file.h). Slots 1-15
 * (`import`, `map`, `abort`, `toString`, `removeAttrs`, `isNull`, `throw`,
 * `derivation`, `baseNameOf`, `dirOf`, `fromTOML`, `placeholder`,
 * `scopedImport`, `derivationStrict`, `fetchGit`) are the names real Nix
 * exposes as bare globals in addition to `builtins.X` -- every primop
 * cppnix registers WITHOUT a `__` prefix gets this treatment
 * automatically (`EvalState::addPrimOp`, `eval.cc`); see builtins.h's
 * ll_make_import_primop comment, builtins_toml.c's
 * ll_make_fromTOML_primop comment, and builtins_fetchgit.c's own
 * header comment (M82) for the fuller picture (including real ones
 * still not implemented at all: `fetchTarball`/`fetchMercurial`/
 * `fetchTree`/`break`). compiler.c's root ScopeFrame
 * (init_builtins_frame) must list the same names in the same order. `up`
 * is the caller's choice (NULL for the ordinary case, or scopedImport's
 * own extra scope-attrs frame). */
/* `skip_derivation_wrapper`: slot 8 (`derivation`) is normally
 * ll_make_derivation_wrapper_value() (M58), which itself lexes,
 * parses, and evaluates a small embedded .nix file -- and THAT
 * evaluation calls back into this very function to build ITS OWN root
 * env, which would try to construct slot 8 again, forever. Passing 1
 * breaks that cycle by using the plain `derivationStrict` primop in
 * slot 8 instead for that one bootstrap evaluation; harmless, since the
 * wrapper source never references bare `derivation` itself, only
 * `derivationStrict` (slot 14). The ordinary path (every other caller)
 * passes 0 and gets the real wrapper, same as before this existed. */
static Env *make_root_env_ex(Env *up, int skip_derivation_wrapper) {
    Env *root_env = ll_env_alloc(up, 16);
    root_env->values[0] = skip_derivation_wrapper ? ll_make_builtins_attrs_for_derivation_bootstrap()
                                                   : ll_make_builtins_attrs();
    root_env->values[1] = ll_make_import_primop();
    root_env->values[2] = ll_make_map_primop();
    root_env->values[3] = ll_make_abort_primop();
    root_env->values[4] = ll_make_toString_primop();
    root_env->values[5] = ll_make_removeAttrs_primop();
    root_env->values[6] = ll_make_isNull_primop();
    root_env->values[7] = ll_make_throw_primop();
    root_env->values[8] = skip_derivation_wrapper ? ll_make_derivation_strict_primop()
                                                   : ll_make_derivation_wrapper_value();
    root_env->values[9] = ll_make_baseNameOf_primop();
    root_env->values[10] = ll_make_dirOf_primop();
    root_env->values[11] = ll_make_fromTOML_primop();
    root_env->values[12] = ll_make_placeholder_primop();
    root_env->values[13] = ll_make_scopedImport_primop();
    root_env->values[14] = ll_make_derivation_strict_primop(); /* M58: derivationStrict, newly bare-global */
    root_env->values[15] = ll_make_fetchGit_primop(); /* M82: fetchGit, newly bare-global */
    return root_env;
}

static Env *make_root_env(Env *up) { return make_root_env_ex(up, 0); }

/* M59: see eval_file.h's own comment on ll_eval_top_level_file. Set
 * once (guarded), from a strdup'd copy -- NOT the `resolved` pointer
 * ll_eval_file itself frees before returning -- so it stays valid for
 * the rest of the process, including during main.c's post-return
 * print_value(), which can still force thunks (and therefore still run
 * `unsafeGetAttrPos`) after ll_eval_file's own `resolved`/`base_dir`
 * have already been freed. */
static char *g_top_level_file = NULL;

const char *ll_eval_top_level_file(void) {
    return g_top_level_file;
}

Value ll_eval_file(const char *path) {
    struct stat st;
    char *resolved = resolve_file_path(path, &st);

    if (!g_top_level_file) {
        size_t len = strlen(resolved);
        g_top_level_file = malloc(len + 1);
        memcpy(g_top_level_file, resolved, len + 1);
    }

    Value cached;
    if (import_cache_find(resolved, &cached)) {
        free(resolved);
        return cached;
    }

    /* M92: give an in-flight background prefetch producer (see
     * bytecode_cache.h) a chance to finish before falling back to a
     * synchronous compile -- no-op if prefetching is disabled or no
     * producer was started for this exact path. */
    ll_bytecode_prefetch_wait(resolved);

    /* M102 (Finding 1): try the CHEAP fast-path cache lookup first --
     * `st` above is already resolve_file_path's own stat result, so
     * this costs no additional syscall in the common case. On a
     * confirmed-fresh hit this skips dir_of()/read_file() entirely: no
     * source-file open/read at all, just the one open+read of the
     * cache entry itself (ll_bytecode_cache_lookup_fast's own doing). */
    int64_t mtime_sec = (int64_t)st.st_mtim.tv_sec;
    int64_t mtime_nsec = (int64_t)st.st_mtim.tv_nsec;
    int64_t fsize = (int64_t)st.st_size;
    int entry_exists = 0;
    IrProgram *prog = ll_bytecode_cache_lookup_fast(resolved, mtime_sec, mtime_nsec, fsize, &entry_exists);

    char *base_dir = NULL;
    if (!prog) {
        /* Either disabled/no-entry (plain miss) or mtime/size drifted
         * (ambiguous) -- either way we now need the source bytes, the
         * same read an ordinary cache-miss compile would always have
         * needed, no worse than before this fix. */
        base_dir = dir_of(resolved);
        size_t src_len;
        char *src = read_file(resolved, &src_len);

        if (entry_exists) {
            /* An entry exists but didn't match on mtime/size -- try the
             * content-hash fallback before giving up and recompiling;
             * see ll_bytecode_cache_lookup_verify's own comment. This
             * is expected to fire every run for exactly one file in a
             * typical nixpkgs closure: compiler.c's embedded
             * `<nix/fetchurl.nix>` corepkgs stub is rewritten fresh to a
             * fixed /tmp path (LL_FETCHURL_TEMP_PATH) on every asnix
             * invocation -- same content every time, but a new mtime by
             * construction -- confirmed directly while verifying this
             * fix (`LL_BYTECODE_CACHE_STATS=1` on ffmpeg's real
             * 1056-file closure consistently showed "1055 fast-path, 1
             * content-verified", never a 1056/0 split, and the one
             * mismatching path was always this stub). A real, harmless
             * instance of the mtime+size heuristic's documented
             * tradeoff: never a wrong answer, just a skipped fast path
             * for this one file, forever (self-heal can't fix a file
             * whose mtime changes on every single read). */
            prog = ll_bytecode_cache_lookup_verify(resolved, src, src_len, mtime_sec, mtime_nsec, fsize);
        }
        if (!prog) {
            Ast *ast = ll_parse(src);
            prog = ll_compile(ast, base_dir, resolved);
            ll_bytecode_cache_store(resolved, src, src_len, mtime_sec, mtime_nsec, fsize, prog);
            ll_bytecode_prefetch_imports(ast, base_dir);
            ll_ast_arena_reset(); /* perf: ast is dead past this point -- see ast.c */
        }
        /* perf: src is dead past this point either way -- consumed by
         * lex/parse/compile/cache_store above, or by
         * ll_bytecode_cache_lookup_verify's own content hash if that
         * path produced prog instead; nothing downstream (the IrProgram,
         * the Ast already reset above) retains a pointer into it. Never
         * freed before this fix -- a ~24 MB leak across a real nixpkgs
         * closure's ~1000+ imported files.
         *
         * Lazy-thunk-compile experiment (hazard #3's simple-but-correct
         * fix, ported from asnix, see compiler.h's own comment on
         * ll_lazy_thunk_enabled): when on, a not-yet-forced stub's
         * Ast string fields may still alias this buffer, so skip
         * freeing it. */
        if (!ll_lazy_thunk_enabled) free(src);
    }
    Env *root_env = make_root_env(NULL);
    Value result = ll_eval(prog, root_env);
    import_cache_insert(resolved, result);
    if (!ll_lazy_thunk_enabled) free(resolved);
    if (!ll_lazy_thunk_enabled) free(base_dir);
    return result;
}

/* M92: the fork()'d bytecode-prefetch producer's entire job (see
 * bytecode_cache.h's own comment on ll_bytecode_prefetch_imports for
 * why this runs in a separate PROCESS, not a thread, and why that makes
 * this function safe to just call the normal, unmodified resolve/read/
 * lex/parse/compile pipeline with zero synchronization). Deliberately
 * does NOT touch g_import_cache/g_top_level_file or call ll_eval --
 * this is cache-WARMING only, never evaluation; the only observable
 * effect on anything outside this one forked process is whatever
 * ll_bytecode_cache_store writes to disk, read back later by the
 * parent's own ordinary ll_bytecode_cache_lookup call. Any failure
 * (file doesn't exist, parse/compile error, cache store refused) just
 * means this child exits having cached nothing -- identical, from the
 * parent's point of view, to never having been forked at all. */
void ll_prefetch_compile_and_cache(const char *path) {
    struct stat st;
    char *resolved = resolve_file_path(path, &st);
    char *base_dir = dir_of(resolved);
    size_t src_len;
    char *src = read_file(resolved, &src_len);
    Ast *ast = ll_parse(src);
    IrProgram *prog = ll_compile(ast, base_dir, resolved);
    ll_bytecode_cache_store(resolved, src, src_len, (int64_t)st.st_mtim.tv_sec, (int64_t)st.st_mtim.tv_nsec,
                                (int64_t)st.st_size, prog);
    ll_ast_arena_reset(); /* perf: ast is dead past this point -- see ast.c (also moot -- this forked child exits right after) */
    free(src); /* perf: see ll_eval_file's identical free(src) comment (also moot here) */
}

/* M58: the one caller that needs make_root_env_ex's cycle-breaking
 * `skip_derivation_wrapper=1` path -- see that function's comment. */
Value ll_eval_file_for_derivation_bootstrap(const char *path) {
    char *resolved, *base_dir, *src;
    Ast *ast = parse_file(path, &resolved, &base_dir, &src);
    IrProgram *prog = ll_compile(ast, base_dir, resolved);
    ll_ast_arena_reset(); /* perf: ast is dead past this point -- see ast.c */
    if (!ll_lazy_thunk_enabled) free(src); /* perf: see ll_eval_file's identical free(src) comment */
    Env *root_env = make_root_env_ex(NULL, 1);
    Value result = ll_eval(prog, root_env);
    if (!ll_lazy_thunk_enabled) free(resolved);
    if (!ll_lazy_thunk_enabled) free(base_dir);
    return result;
}

/* M54: see eval_file.h/compiler.c's ll_compile_scoped for the full
 * rationale. `scope`'s attribute NAMES (not values -- those stay lazy,
 * same as any other attrset field) become an extra, innermost lexical
 * ScopeFrame, resolved at compile time; at runtime that frame is one more
 * Env hop (`scope_env`, holding `scope`'s own field values in the same
 * order its names were listed) wrapped around the ordinary 14-slot root
 * env (`up = scope_env`'s own `up`). */
Value ll_eval_file_scoped(const char *path, Value scope) {
    char *resolved, *base_dir, *src;
    Ast *ast = parse_file(path, &resolved, &base_dir, &src);

    int64_t n = ll_bshared_attrs_count(scope);
    const char **names = malloc((size_t)(n > 0 ? n : 1) * sizeof(const char *));
    size_t *lens = malloc((size_t)(n > 0 ? n : 1) * sizeof(size_t));
    for (int64_t i = 0; i < n; i++) {
        uint32_t sym = ll_bshared_attrs_sym(scope, i);
        names[i] = ll_sym_name(sym, &lens[i]);
    }

    IrProgram *prog = ll_compile_scoped(ast, base_dir, resolved, names, lens, (size_t)n);
    ll_ast_arena_reset(); /* perf: ast is dead past this point -- see ast.c */
    if (!ll_lazy_thunk_enabled) free(src); /* perf: see ll_eval_file's identical free(src) comment --
                 * names[]/lens[] above come from ll_sym_name's own
                 * interned storage, not from src, so nothing here still
                 * points into it either. */

    Env *root_env = make_root_env(NULL);
    Env *scope_env = ll_env_alloc(root_env, n);
    for (int64_t i = 0; i < n; i++) scope_env->values[i] = ll_bshared_attrs_value(scope, i);

    Value result = ll_eval(prog, scope_env);

    free(names);
    free(lens);
    if (!ll_lazy_thunk_enabled) free(resolved);
    if (!ll_lazy_thunk_enabled) free(base_dir);
    return result;
}

/* M57i: CLI `--arg`/`--argstr` expression evaluation (main.c) -- the same
 * lex/parse/compile/fresh-root-env/eval pipeline `ll_eval_file` runs
 * over a file's contents, but over a caller-supplied source STRING with
 * no file of its own; relative path literals in `src` resolve against the
 * process's cwd (confirmed: real Nix's own `--arg x ./foo.nix` resolves
 * `./foo.nix` against the invoking shell's cwd, not any particular
 * file's directory). */
Value ll_eval_expr_source(const char *src) {
    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd)) {
        fprintf(stderr, "ll: cannot determine the current directory\n");
        exit(1);
    }
    Ast *ast = ll_parse(src);
    IrProgram *prog = ll_compile(ast, cwd, NULL);
    ll_ast_arena_reset(); /* perf: ast is dead past this point -- see ast.c */
    Env *root_env = make_root_env(NULL);
    return ll_eval(prog, root_env);
}
