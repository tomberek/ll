/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
/* M82: `builtins.fetchGit` -- cppnix's `prim_fetchGit`/`fetchTree`
 * (~/nix/src/libexpr/primops/fetchTree.cc, isFetchGit=true) wrapping
 * libfetchers' "git" input type (~/nix/src/libfetchers/git.cc), read in
 * full before writing this. Found needed chasing `d.nix` past M81:
 * `pkgs/build-support/rust/import-cargo-lock.nix` (reached transitively
 * through ffmpeg's rust-toolchain dependency chain) references the bare
 * top-level `fetchGit` identifier inside one branch of an `if`
 * (`else if allowBuiltinFetchGit then fetchGit {...} else missingHash`)
 * -- and this codebase COMPILES an entire imported file's whole AST
 * up front (compiler.c's `compile_expr`, called once per file from
 * `asnix_eval_file`), including both arms of every `if`, not just the
 * one that ends up running. compiler.c's AST_VAR case resolves every
 * free variable against the static scope chain *at compile time*
 * (confirmed: that's a direct port of real Nix's own `bindVars`, which
 * is equally whole-file/static there -- see compiler.c's own "undefined
 * variable" fprintf site) -- so merely COMPILING this file (which
 * happens for every package that transitively imports it, regardless of
 * whether `allowBuiltinFetchGit` is ever true at runtime) already needs
 * `fetchGit` to resolve to *something* as a known bare-global name, same
 * as `import`/`throw`/`derivation`/etc (builtins.h's own comment on real
 * Nix's "every primop registered without a `__` prefix is a bare
 * global" rule -- `fetchGit` is explicitly on that grepped list).
 *
 * Confirmed by grepping the actual reachable nixpkgs tree
 * (`/home/tbereknyei/nixpkgs`) for every call site of `importCargoLock`/
 * `allowBuiltinFetchGit`: nothing anywhere sets `allowBuiltinFetchGit =
 * true` (it only ever takes its own `? false` default), so the
 * `fetchGit {...}` *call* itself is never actually reached at runtime
 * while evaluating `(import nixpkgs {}).ffmpeg.outPath` -- the crash
 * this milestone fixes is a pure static-name-resolution problem, not a
 * "the fetch itself needs to actually run and produce a real result"
 * problem. That said, this milestone implements a REAL, working
 * `fetchGit` (not just a stub that satisfies the static check) per this
 * project's standing discipline of not leaving a builtin half-real when
 * a reasonably-scoped genuine implementation is reachable -- verified
 * directly against `nix-instantiate --eval --strict` with a throwaway
 * local git repo (see docs/architecture.md's M82 notes for the exact
 * transcript) before being trusted at all, same as every other hash/
 * path-formula milestone in this project's history.
 *
 * Shells out to a real `git` binary (popen/pclose, "a plain external
 * process", same category of escape hatch this project's own
 * docs/abi.md already describes for primop calls in general) to do a
 * REAL bare clone + `git archive` of the requested rev -- there is no
 * git-object-store implementation in this codebase and building one
 * would be wildly disproportionate to this feature's actual payoff here.
 * The fetched tree is then run through M75/M80's existing NAR-hash
 * store-copy machinery (`nar.c`'s `asnix_copy_path_to_store_ext`/the new
 * `asnix_hash_path_nar_sha256`, nar.h) -- the exact same formula a
 * fixed-output derivation or `builtins.path` already uses, not a
 * separately-derived one, so `outPath`/`narHash` here are genuinely
 * real, byte-for-byte-verifiable store paths/hashes, not placeholders.
 *
 * Supported, real, and verified against `nix-instantiate --eval
 * --strict`:
 *   - Bare string/path shorthand (`fetchGit "/local/repo"`,
 *     `fetchGit "https://example.com/foo.git"`) -- resolves to the
 *     clone's default branch HEAD, same as real Nix.
 *   - Attrset form: `url` (required), `ref`, `rev`, `shallow`
 *     (`--depth 1` on the clone), `submodules` (accepted and echoed
 *     back in the result attrset, matching real Nix's own always-
 *     present `submodules` field for the "git" input type -- see the
 *     scope cut below for what's NOT real about it).
 *   - Result attrset: `outPath` (real store path, with the same Opaque
 *     string-context tag `builtins.path`'s result carries), `narHash`
 *     (real SRI `sha256-<base64>`, from actually NAR-hashing the fetched
 *     tree), `rev`, `shortRev` (first 7 hex chars, matching real Nix's
 *     `Hash::gitShortRev`), `revCount` (`git rev-list --count`),
 *     `lastModified`/`lastModifiedDate` (the resolved commit's own
 *     author... actually committer time, `%ct`, formatted exactly like
 *     real Nix's `std::put_time(gmtime(...), "%Y%m%d%H%M%S")`).
 *   - A `rev` not reachable from any default-cloned branch/tag is
 *     retried once via `git fetch origin <rev>` (works against modern
 *     GitHub-style servers that allow fetching an arbitrary reachable
 *     SHA) before giving up -- a real, if best-effort, stand-in for
 *     real Nix's own `allRefs`-driven "fetch everything" behavior,
 *     without this codebase needing to replicate libfetchers' exact
 *     refspec/cache logic.
 *
 * Deliberate, documented scope cuts (none of these are exercised by
 * anything this project has actually needed to evaluate so far --
 * confirmed by the "nothing sets allowBuiltinFetchGit=true" grep above
 * -- so, per this project's standing "support what's actually
 * reachable" convention, left as real gaps rather than guessed at):
 *   - `submodules = true` does NOT actually recurse into/fetch
 *     submodule content -- the returned `submodules` field is just
 *     echoed from the argument, same shape as real Nix's attrset, but
 *     the hashed/copied tree itself won't contain real submodule
 *     content the way real Nix's `GitRepo::getSubmodules` would fetch.
 *   - Local *dirty* working trees (real Nix's `fetchGit ./work-dir`
 *     with no `ref`/`rev`, hashing whatever uncommitted changes are
 *     currently on disk and returning an empty-sha1 `rev`/`dirtyRev`
 *     pair) aren't modeled -- every repo this fetches from (local path
 *     or remote URL) is treated as a plain clone target and always
 *     resolves to a real committed rev, never "the literal working
 *     directory's current (possibly dirty) state".
 *   - `verifyCommit`/`publicKeys` (signed-commit verification),
 *     `exportIgnore` as an explicit override (real Nix's default-true
 *     behavior for non-submodule fetches IS honored here for free,
 *     since `git archive` itself already honors `export-ignore`
 *     `.gitattributes` entries -- only an explicit `exportIgnore =
 *     false` override to SUPPRESS that isn't), `lfs` smudging, a
 *     pinned `narHash`/`lastModified` argument to skip re-fetching, and
 *     the registry-lookup/flake-ref-shorthand forms (`"github:..."`)
 *     are all accepted-and-ignored or simply not special-cased --
 *     unknown attrset keys besides the handful above are silently
 *     accepted (not validated against real Nix's own fetcher-specific
 *     `allowedAttrs` schema).
 *   - Network/offline failures from the underlying `git` invocation are
 *     reported as a plain fatal error (this project's standing
 *     convention for every other builtin), not real Nix's own
 *     catchable `EvalError` class.
 */

#define _POSIX_C_SOURCE 200809L /* strndup, mkdtemp -- same macro builtins_path.c/nar.c already use */

#include "builtins_fetchgit.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "asnix/ir.h" /* asnix_force_for_print */
#include "base64.h"
#include "builtins_shared.h"
#include "nar.h"
#include "string_context.h"
#include "symtab.h"
#include "value_util.h"

static _Noreturn void fg_fatal(const char *msg) {
    fprintf(stderr, "asnix: fetchGit: %s\n", msg);
    exit(1);
}

/* Wraps `s` in single quotes for safe embedding in a `/bin/sh -c`
 * command line (popen's own shell), escaping any embedded `'` the
 * standard POSIX-shell way (`'\''`). Caller-freed. */
static char *fg_shq(const char *s) {
    size_t len = strlen(s);
    char *out = malloc(len * 4 + 3);
    char *p = out;
    *p++ = '\'';
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\'') {
            *p++ = '\'';
            *p++ = '\\';
            *p++ = '\'';
            *p++ = '\'';
        } else {
            *p++ = s[i];
        }
    }
    *p++ = '\'';
    *p = '\0';
    return out;
}

/* Runs `cmd` via popen, discarding stdout, returning its exit status (or
 * -1 if it couldn't even be started/didn't exit normally). */
static int fg_run(const char *cmd) {
    FILE *f = popen(cmd, "r");
    if (!f) return -1;
    char buf[4096];
    while (fread(buf, 1, sizeof(buf), f) > 0) { }
    int status = pclose(f);
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

/* Same as fg_run, but captures stdout (trimmed of trailing CR/LF) into
 * `out` (capacity `out_cap`, always left NUL-terminated). */
static int fg_run_capture(const char *cmd, char *out, size_t out_cap) {
    FILE *f = popen(cmd, "r");
    if (!f) return -1;
    size_t n = out_cap ? fread(out, 1, out_cap - 1, f) : 0;
    if (out_cap) out[n] = '\0';
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = '\0';
    int status = pclose(f);
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

static char *fg_dup_string_attr(Value v, const char *who) {
    v = asnix_force_for_print(v);
    int64_t len;
    const char *bytes;
    asnix_bshared_string_bytes(v, &bytes, &len, who);
    char *s = malloc((size_t)len + 1);
    memcpy(s, bytes, (size_t)len);
    s[len] = '\0';
    return s;
}

static int fg_bool_attr(Value v, const char *who) {
    v = asnix_force_for_print(v);
    if (asnix_tag_of(v) != ASNIX_TAG_IMM
        || (asnix_imm_subtag(v) != ASNIX_IMM_TRUE && asnix_imm_subtag(v) != ASNIX_IMM_FALSE)) {
        fprintf(stderr, "asnix: fetchGit: '%s' must be a boolean\n", who);
        exit(1);
    }
    return asnix_imm_subtag(v) == ASNIX_IMM_TRUE;
}

Value asnix_prim_fetchGit(const Value *args, int64_t argc) {
    (void)argc;
    Value arg = asnix_force_for_print(args[0]);

    char *url = NULL;
    char *ref = NULL;
    char *rev = NULL;
    int shallow = 0;
    int submodules = 0;

    if (asnix_tag_of(arg) == ASNIX_TAG_ATTRS) {
        int64_t n = asnix_bshared_attrs_count(arg);
        for (int64_t i = 0; i < n; i++) {
            uint32_t sym = asnix_bshared_attrs_sym(arg, i);
            size_t klen;
            const char *kname = asnix_sym_name(sym, &klen);
            Value val = asnix_bshared_attrs_value(arg, i);
            if (klen == 3 && memcmp(kname, "url", 3) == 0) {
                url = fg_dup_string_attr(val, "url");
            } else if (klen == 3 && memcmp(kname, "ref", 3) == 0) {
                ref = fg_dup_string_attr(val, "ref");
            } else if (klen == 3 && memcmp(kname, "rev", 3) == 0) {
                rev = fg_dup_string_attr(val, "rev");
            } else if (klen == 7 && memcmp(kname, "shallow", 7) == 0) {
                shallow = fg_bool_attr(val, "shallow");
            } else if (klen == 10 && memcmp(kname, "submodules", 10) == 0) {
                submodules = fg_bool_attr(val, "submodules");
            } else if (klen == 4 && memcmp(kname, "name", 4) == 0) {
                fg_fatal("argument 'name' isn't supported in call to 'fetchGit'");
            } else if (klen == 4 && memcmp(kname, "type", 4) == 0) {
                fg_fatal("unexpected argument 'type'");
            }
            /* Everything else (allRefs, exportIgnore, verifyCommit,
             * publicKeys, narHash, lastModified, lfs, ...) is accepted
             * and ignored -- see this file's header comment's scope-cut
             * list. */
        }
        if (!url) fg_fatal("argument 'url' is missing in call to 'fetchGit'");
    } else {
        url = fg_dup_string_attr(arg, "fetchGit");
    }

    char scratch_template[] = "/tmp/asnix-fetchgit-XXXXXX";
    char *scratch = mkdtemp(scratch_template);
    if (!scratch) fg_fatal("failed to create a scratch directory under /tmp");

    char clonedir[300], workdir[300];
    snprintf(clonedir, sizeof(clonedir), "%s/repo.git", scratch);
    snprintf(workdir, sizeof(workdir), "%s/tree", scratch);
    if (mkdir(workdir, 0700) != 0) fg_fatal("failed to create a scratch tree directory");

    char *url_q = fg_shq(url);
    char *clonedir_q = fg_shq(clonedir);
    char clone_cmd[2300];
    snprintf(clone_cmd, sizeof(clone_cmd), "git clone --quiet --bare %s%s %s 2>/dev/null",
              shallow ? "--depth 1 " : "", url_q, clonedir_q);
    if (fg_run(clone_cmd) != 0) {
        char msg[400];
        snprintf(msg, sizeof(msg), "failed to clone '%s' (is it a reachable git repository?)", url);
        fg_fatal(msg);
    }

    char resolved_rev[128] = { 0 };
    if (rev) {
        char *rev_q = fg_shq(rev);
        char check_cmd[700];
        snprintf(check_cmd, sizeof(check_cmd), "git --git-dir=%s cat-file -e %s 2>/dev/null", clonedir_q, rev_q);
        if (fg_run(check_cmd) != 0) {
            /* Not reachable from the default clone's fetched branches/
             * tags -- best-effort retry, fetching that exact commit by
             * SHA directly (see header comment's allRefs note). */
            char fetch_cmd[700];
            snprintf(fetch_cmd, sizeof(fetch_cmd), "git --git-dir=%s fetch --quiet origin %s 2>/dev/null", clonedir_q,
                      rev_q);
            fg_run(fetch_cmd);
        }
        free(rev_q);
        snprintf(resolved_rev, sizeof(resolved_rev), "%s", rev);
    } else {
        const char *target = ref ? ref : "HEAD";
        char *target_q = fg_shq(target);
        char rp_cmd[700];
        snprintf(rp_cmd, sizeof(rp_cmd), "git --git-dir=%s rev-parse %s 2>/dev/null", clonedir_q, target_q);
        free(target_q);
        if (fg_run_capture(rp_cmd, resolved_rev, sizeof(resolved_rev)) != 0 || resolved_rev[0] == '\0') {
            char msg[300];
            snprintf(msg, sizeof(msg), "failed to resolve '%s' to a revision", target);
            fg_fatal(msg);
        }
    }

    char *rev_q2 = fg_shq(resolved_rev);
    char tarpath[330];
    snprintf(tarpath, sizeof(tarpath), "%s/tree.tar", scratch);
    char *tarpath_q = fg_shq(tarpath);
    char archive_cmd[900];
    snprintf(archive_cmd, sizeof(archive_cmd), "git --git-dir=%s archive -o %s %s 2>/dev/null", clonedir_q, tarpath_q,
              rev_q2);
    if (fg_run(archive_cmd) != 0) fg_fatal("'git archive' failed -- the requested rev could not be fetched");

    char *workdir_q = fg_shq(workdir);
    char extract_cmd[900];
    snprintf(extract_cmd, sizeof(extract_cmd), "tar -xf %s -C %s", tarpath_q, workdir_q);
    if (fg_run(extract_cmd) != 0) fg_fatal("failed to extract the fetched git tree");

    int64_t rev_count = 0;
    {
        char rc_cmd[700];
        snprintf(rc_cmd, sizeof(rc_cmd), "git --git-dir=%s rev-list --count %s 2>/dev/null", clonedir_q, rev_q2);
        char rc_buf[32];
        if (fg_run_capture(rc_cmd, rc_buf, sizeof(rc_buf)) == 0 && rc_buf[0]) rev_count = strtoll(rc_buf, NULL, 10);
    }

    int have_last_modified = 0;
    int64_t last_modified = 0;
    char last_modified_date[32] = { 0 };
    {
        char lm_cmd[700];
        snprintf(lm_cmd, sizeof(lm_cmd), "git --git-dir=%s log -1 --format=%%ct %s 2>/dev/null", clonedir_q, rev_q2);
        char lm_buf[32];
        if (fg_run_capture(lm_cmd, lm_buf, sizeof(lm_buf)) == 0 && lm_buf[0]) {
            have_last_modified = 1;
            last_modified = strtoll(lm_buf, NULL, 10);
            time_t t = (time_t)last_modified;
            struct tm tmv;
            gmtime_r(&t, &tmv);
            strftime(last_modified_date, sizeof(last_modified_date), "%Y%m%d%H%M%S", &tmv);
        }
    }

    char short_rev[8];
    size_t revlen = strlen(resolved_rev);
    size_t sl = revlen < 7 ? revlen : 7;
    memcpy(short_rev, resolved_rev, sl);
    short_rev[sl] = '\0';

    uint8_t digest[32];
    if (asnix_hash_path_nar_sha256(workdir, NULL, NULL, digest) != 0)
        fg_fatal("failed to load libcrypto -- enter the flake devShell");
    size_t b64len = asnix_base64_encoded_length(32);
    char narhash_b64[64];
    asnix_base64_encode(digest, 32, narhash_b64);
    char narhash_str[80];
    snprintf(narhash_str, sizeof(narhash_str), "sha256-%.*s", (int)b64len, narhash_b64);

    char out_path[300];
    asnix_copy_path_to_store_ext(workdir, "source", 1, NULL, NULL, NULL, out_path, sizeof(out_path));

    /* Best-effort cleanup -- a leaked /tmp scratch dir on failure is
     * harmless (every fatal path above already exit()s first). */
    char *scratch_q = fg_shq(scratch);
    char rm_cmd[350];
    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", scratch_q);
    fg_run(rm_cmd);

    free(url_q);
    free(clonedir_q);
    free(rev_q2);
    free(tarpath_q);
    free(workdir_q);
    free(scratch_q);
    free(url);
    free(ref);
    free(rev);

    Value out_path_str = asnix_make_string_value(out_path, strlen(out_path));
    AsnixStringContext ctx;
    asnix_ctx_init(&ctx);
    asnix_ctx_add_opaque(&ctx, out_path);
    asnix_string_attach_context(out_path_str, &ctx);

    uint32_t syms[8];
    Value vals[8];
    int64_t k = 0;
    syms[k] = asnix_intern("outPath", 7);
    vals[k++] = out_path_str;
    syms[k] = asnix_intern("narHash", 7);
    vals[k++] = asnix_make_string_value(narhash_str, strlen(narhash_str));
    syms[k] = asnix_intern("rev", 3);
    vals[k++] = asnix_make_string_value(resolved_rev, strlen(resolved_rev));
    syms[k] = asnix_intern("shortRev", 8);
    vals[k++] = asnix_make_string_value(short_rev, strlen(short_rev));
    syms[k] = asnix_intern("revCount", 8);
    vals[k++] = asnix_make_int(rev_count);
    syms[k] = asnix_intern("submodules", 10);
    vals[k++] = submodules ? ASNIX_TRUE : ASNIX_FALSE;
    if (have_last_modified) {
        syms[k] = asnix_intern("lastModified", 12);
        vals[k++] = asnix_make_int(last_modified);
        syms[k] = asnix_intern("lastModifiedDate", 16);
        vals[k++] = asnix_make_string_value(last_modified_date, strlen(last_modified_date));
    }

    return asnix_bshared_make_attrs(syms, vals, k);
}

Value asnix_make_fetchGit_primop(void) {
    return asnix_bshared_make_primop((void *)asnix_prim_fetchGit, 1);
}
