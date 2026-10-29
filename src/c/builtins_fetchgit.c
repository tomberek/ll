/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
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

#include "ll/ir.h" /* ll_force_for_print */
#include "base64.h"
#include "builtins_shared.h"
#include "nar.h"
#include "string_context.h"
#include "symtab.h"
#include "value_util.h"

static _Noreturn void fg_fatal(const char *msg) {
    fprintf(stderr, "ll: fetchGit: %s\n", msg);
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
    v = ll_force_for_print(v);
    int64_t len;
    const char *bytes;
    ll_bshared_string_bytes(v, &bytes, &len, who);
    char *s = malloc((size_t)len + 1);
    memcpy(s, bytes, (size_t)len);
    s[len] = '\0';
    return s;
}

static int fg_bool_attr(Value v, const char *who) {
    v = ll_force_for_print(v);
    if (ll_tag_of(v) != LL_TAG_IMM
        || (ll_imm_subtag(v) != LL_IMM_TRUE && ll_imm_subtag(v) != LL_IMM_FALSE)) {
        fprintf(stderr, "ll: fetchGit: '%s' must be a boolean\n", who);
        exit(1);
    }
    return ll_imm_subtag(v) == LL_IMM_TRUE;
}

Value ll_prim_fetchGit(const Value *args, int64_t argc) {
    (void)argc;
    Value arg = ll_force_for_print(args[0]);

    char *url = NULL;
    char *ref = NULL;
    char *rev = NULL;
    int shallow = 0;
    int submodules = 0;

    if (ll_tag_of(arg) == LL_TAG_ATTRS) {
        int64_t n = ll_bshared_attrs_count(arg);
        for (int64_t i = 0; i < n; i++) {
            uint32_t sym = ll_bshared_attrs_sym(arg, i);
            size_t klen;
            const char *kname = ll_sym_name(sym, &klen);
            Value val = ll_bshared_attrs_value(arg, i);
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
             * and ignored. */
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
             * SHA directly. */
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
    if (ll_hash_path_nar_sha256(workdir, NULL, NULL, digest) != 0)
        fg_fatal("failed to load libcrypto -- enter the flake devShell");
    size_t b64len = ll_base64_encoded_length(32);
    char narhash_b64[64];
    ll_base64_encode(digest, 32, narhash_b64);
    char narhash_str[80];
    snprintf(narhash_str, sizeof(narhash_str), "sha256-%.*s", (int)b64len, narhash_b64);

    char out_path[300];
    ll_copy_path_to_store_ext(workdir, "source", 1, NULL, NULL, NULL, out_path, sizeof(out_path));

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

    Value out_path_str = ll_make_string_value(out_path, strlen(out_path));
    LlStringContext ctx;
    ll_ctx_init(&ctx);
    ll_ctx_add_opaque(&ctx, out_path);
    ll_string_attach_context(out_path_str, &ctx);

    uint32_t syms[8];
    Value vals[8];
    int64_t k = 0;
    syms[k] = ll_intern("outPath", 7);
    vals[k++] = out_path_str;
    syms[k] = ll_intern("narHash", 7);
    vals[k++] = ll_make_string_value(narhash_str, strlen(narhash_str));
    syms[k] = ll_intern("rev", 3);
    vals[k++] = ll_make_string_value(resolved_rev, strlen(resolved_rev));
    syms[k] = ll_intern("shortRev", 8);
    vals[k++] = ll_make_string_value(short_rev, strlen(short_rev));
    syms[k] = ll_intern("revCount", 8);
    vals[k++] = ll_make_int(rev_count);
    syms[k] = ll_intern("submodules", 10);
    vals[k++] = submodules ? LL_TRUE : LL_FALSE;
    if (have_last_modified) {
        syms[k] = ll_intern("lastModified", 12);
        vals[k++] = ll_make_int(last_modified);
        syms[k] = ll_intern("lastModifiedDate", 16);
        vals[k++] = ll_make_string_value(last_modified_date, strlen(last_modified_date));
    }

    return ll_bshared_make_attrs(syms, vals, k);
}

Value ll_make_fetchGit_primop(void) {
    return ll_bshared_make_primop((void *)ll_prim_fetchGit, 1);
}
