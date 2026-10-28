/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "compiler.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "asnix/arena.h"
#include "asnix/heap.h"
#include "asnix/irbuild.h"
#include "asnix/value.h"
#include "builtins_list2.h"  /* M110: asnix_prim_{elem,any,all}2_impl */
#include "builtins_m23.h"    /* M110: asnix_prim_intersectAttrs2_impl */
#include "builtins_regex.h"  /* M110: asnix_prim_match2_impl */
#include "builtins_shared.h"
#include "eval_file.h" /* M80: asnix_eval_top_level_file, for AST_CURPOS's `file` field */
#include "symtab.h"
#include "tryeval.h"
#include "value_util.h"

/* AST_PATH resolution (import-paths). `base_dir` is constant for one whole
 * asnix_compile() call (every path literal in a given file resolves
 * against that *file's* directory, not wherever the thunk/lambda body
 * containing it happens to run later) -- threading it through every
 * compile_expr recursive call as an explicit parameter would touch every
 * call site for no benefit, so it's stashed here instead. Safe as a
 * single global rather than a stack: asnix_compile() always runs to
 * completion (there is no `call asnix_compile` while another
 * asnix_compile() frame is still on the C stack) before asnix_eval() ever
 * starts running the IR that might later trigger `import` (which runs a
 * *fresh, separate* asnix_eval_file() call, well after this one
 * returned) -- see src/c/eval_file.c. */
static const char *g_compile_base_dir = NULL;

/* M82: see compiler.h's own comment on asnix_compile's `file_path`
 * parameter -- same lifetime/scoping rules as `g_compile_base_dir` just
 * above (valid for exactly one asnix_compile()/asnix_compile_scoped()
 * call, read only by AST_CURPOS in compile_expr). */
static const char *g_compile_file_path = NULL;

/* Lexically canonicalizes an absolute path: collapses `.` and `..`
 * components and doubled slashes, same as real Nix's `canonPath`
 * (verified against `nix-instantiate --eval 'builtins.toString
 * ./sub/../foo.nix'`), but -- unlike `canonPath` -- never touches the
 * filesystem (no symlink resolution): cheap, and sufficient for every
 * case this project's fixtures exercise. `in` must start with '/'.
 * M57g: moved to value_util.c as `asnix_canon_path` so `asnix_add_values`
 * (`path + string` concatenation) can reuse it too -- see that
 * function's own comment, value_util.h. */
#define canon_path asnix_canon_path

/* Resolves a NIX_PATH search-path entry name (the text inside `<...>`,
 * e.g. "nixpkgs" or "nixpkgs/lib") the same way real Nix's `findFile`
 * does: NIX_PATH is a colon-separated list of either `name=dir` (matched
 * by exact leading-component name) or plain `dir` entries (searched by
 * checking whether `dir/<name>` exists); first match wins. Aborts with a
 * message matching nix-instantiate's wording if nothing matches or
 * NIX_PATH isn't set. */
static char *resolve_search_path(const char *name, size_t name_len) {
    const char *nix_path = getenv("NIX_PATH");
    const char *first_slash = memchr(name, '/', name_len);
    size_t head_len = first_slash ? (size_t)(first_slash - name) : name_len;

    if (nix_path && nix_path[0]) {
        char *copy = malloc(strlen(nix_path) + 1);
        memcpy(copy, nix_path, strlen(nix_path) + 1);
        char *entry = copy;
        while (entry) {
            char *colon = strchr(entry, ':');
            if (colon) *colon = '\0';
            if (*entry) {
                char *eq = strchr(entry, '=');
                const char *dir;
                int matched = 0;
                if (eq) {
                    size_t ename_len = (size_t)(eq - entry);
                    if (ename_len == head_len && memcmp(entry, name, head_len) == 0) matched = 1;
                    dir = eq + 1;
                } else {
                    dir = entry;
                    matched = 1; /* plain dir entries are always probed below */
                }
                if (matched) {
                    size_t dir_len = strlen(dir);
                    size_t cand_len = dir_len + 1 + name_len;
                    char *cand = malloc(cand_len + 1);
                    memcpy(cand, dir, dir_len);
                    cand[dir_len] = '/';
                    memcpy(cand + dir_len + 1, name, name_len);
                    cand[cand_len] = '\0';
                    if (eq) return cand; /* exact `name=dir` match: trust it, no stat needed */
                    struct stat st;
                    if (stat(cand, &st) == 0) return cand;
                    free(cand);
                }
            }
            entry = colon ? colon + 1 : NULL;
        }
        free(copy);
    }

    /* M31: routed through the catchable-throw mechanism, not a direct
     * exit -- real Nix's own `findFile` internally throws `ThrownError`
     * for this exact case (`~/nix/src/libexpr/eval.cc`, confirmed in
     * source: `error<ThrownError>("file '%s' was not found...")`), the
     * same exception class `throw` itself uses, which is why
     * `builtins.tryEval (toString <nameNotInNIX_PATH>)` successfully
     * catches it in real Nix (confirmed directly: nixpkgs's own
     * `pkgs/top-level/impure-overlays.nix` relies on exactly this to
     * fail gracefully when `$NIX_PATH` doesn't define
     * `nixpkgs-overlays`). Falls back to identical write+exit(1)
     * behavior when no `tryEval` is active. */
    static const char fmt[] = "asnix: file '%.*s' was not found in the Nix search path (set $NIX_PATH)\n";
    int needed = snprintf(NULL, 0, fmt, (int)name_len, name); /* exact length first, not guessed */
    char *formatted = malloc((size_t)needed + 1);
    snprintf(formatted, (size_t)needed + 1, fmt, (int)name_len, name);
    asnix_tryeval_raise_or_fatal(formatted, (size_t)needed);
}

/* `<nix/fetchurl.nix>` (M46) is NOT an ordinary search-path lookup in
 * real Nix at all -- confirmed in cppnix's source
 * (`~/nix/src/libexpr/eval.cc`): `corepkgsFS`, an in-memory virtual
 * filesystem, has exactly ONE file ever added to it
 * (`corepkgsFS->addFile(CanonPath("fetchurl.nix"), {#embed
 * "fetchurl.nix"})`, the file's bytes baked directly into the `nix`
 * binary at compile time), and any `<nix/...>` path is routed to this
 * virtual FS (`path.substr(3)`) BEFORE the ordinary `NIX_PATH`-walking
 * logic `resolve_search_path` implements ever runs -- there is no real
 * `corepkgs` directory on disk to point `NIX_PATH` at. Ported the exact
 * file content verbatim from cppnix's own source
 * (`~/nix/src/libexpr/fetchurl.nix`) rather than reconstructing it, to
 * avoid any behavioral drift from what real Nix actually embeds.
 * Written out to a fixed, stable temp path once per process and
 * imported from there like any other path -- reuses 100% of the
 * existing Path/import machinery rather than teaching it about virtual,
 * non-filesystem content; externally indistinguishable for every
 * reachable use (`import <nix/fetchurl.nix> {...}`, never `toString
 * <nix/fetchurl.nix>` itself, which WOULD print differently from real
 * Nix's special `"<nix" .. ">"` display -- a documented, narrower scope
 * cut, not hit by anything this project has evaluated so far). Found
 * needed evaluating real nixpkgs: `pkgs/build-support/fetchurl/
 * default.nix` imports this. */
static const char ASNIX_FETCHURL_NIX_SOURCE[] =
    "{\n"
    "  system ? \"\", # obsolete\n"
    "  url,\n"
    "  hash ? \"\", # an SRI hash\n"
    "\n"
    "  # Legacy hash specification\n"
    "  md5 ? \"\",\n"
    "  sha1 ? \"\",\n"
    "  sha256 ? \"\",\n"
    "  sha512 ? \"\",\n"
    "  outputHash ?\n"
    "    if hash != \"\" then\n"
    "      hash\n"
    "    else if sha512 != \"\" then\n"
    "      sha512\n"
    "    else if sha1 != \"\" then\n"
    "      sha1\n"
    "    else if md5 != \"\" then\n"
    "      md5\n"
    "    else\n"
    "      sha256,\n"
    "  outputHashAlgo ?\n"
    "    if hash != \"\" then\n"
    "      \"\"\n"
    "    else if sha512 != \"\" then\n"
    "      \"sha512\"\n"
    "    else if sha1 != \"\" then\n"
    "      \"sha1\"\n"
    "    else if md5 != \"\" then\n"
    "      \"md5\"\n"
    "    else\n"
    "      \"sha256\",\n"
    "\n"
    "  executable ? false,\n"
    "  unpack ? false,\n"
    "  name ? baseNameOf (toString url),\n"
    "  impure ? false,\n"
    "}:\n"
    "\n"
    "derivation (\n"
    "  {\n"
    "    builder = \"builtin:fetchurl\";\n"
    "\n"
    "    # New-style output content requirements.\n"
    "    outputHashMode = if unpack || executable then \"recursive\" else \"flat\";\n"
    "\n"
    "    inherit\n"
    "      name\n"
    "      url\n"
    "      executable\n"
    "      unpack\n"
    "      ;\n"
    "\n"
    "    system = \"builtin\";\n"
    "\n"
    "    # No need to double the amount of network traffic\n"
    "    preferLocalBuild = true;\n"
    "\n"
    "    # This attribute does nothing; it's here to avoid changing evaluation results.\n"
    "    impureEnvVars = [\n"
    "      \"http_proxy\"\n"
    "      \"https_proxy\"\n"
    "      \"ftp_proxy\"\n"
    "      \"all_proxy\"\n"
    "      \"no_proxy\"\n"
    "    ];\n"
    "\n"
    "    # To make \"nix-prefetch-url\" work.\n"
    "    urls = [ url ];\n"
    "  }\n"
    "  // (if impure then { __impure = true; } else { inherit outputHashAlgo outputHash; })\n"
    ")\n";

#define ASNIX_FETCHURL_TEMP_PATH "/tmp/.asnix-corepkgs-fetchurl.nix"

/* Unlike derivation.c's write_derivation_nix (called at most once per
 * process, from asnix_make_derivation_wrapper_value's own have_cached
 * guard), this one is reached straight from OP_RESOLVE_SEARCH_PATH --
 * once per `<nix/fetchurl.nix>` AST node *evaluation*, not once per
 * process. Real nixpkgs' pkgs/build-support/fetchurl/boot.nix makes
 * `import <nix/fetchurl.nix> {...}` the BODY of a function
 * (`fetchurlBoot`), called fresh for every package's `src = fetchurl
 * {...}` -- hundreds of times evaluating d.nix. Real Nix's own
 * corepkgsFS (see the big comment below) is an in-memory virtual FS, so
 * re-resolving it there costs nothing; porting it as a real on-disk
 * temp file means every one of those hundreds of calls was paying a
 * real fopen+fwrite+fclose (confirmed via strace: 280 rewrites of the
 * same never-changing 1350 bytes evaluating d.nix). The file's content
 * is static for the whole process, so write it once and reuse the
 * path after that -- same have_cached shape as
 * asnix_make_derivation_wrapper_value. */
static const char *write_fetchurl_nix(void) {
    static int written = 0;
    if (written) return ASNIX_FETCHURL_TEMP_PATH;
    FILE *f = fopen(ASNIX_FETCHURL_TEMP_PATH, "w");
    if (!f) {
        fprintf(stderr, "asnix: could not write '%s' for <nix/fetchurl.nix>\n", ASNIX_FETCHURL_TEMP_PATH);
        exit(1);
    }
    fwrite(ASNIX_FETCHURL_NIX_SOURCE, 1, sizeof(ASNIX_FETCHURL_NIX_SOURCE) - 1, f);
    fclose(f);
    written = 1;
    return ASNIX_FETCHURL_TEMP_PATH;
}

/* `<name>` search-path resolution, called from OP_RESOLVE_SEARCH_PATH
 * (M31) -- unlike an ordinary path literal (`./foo.nix`, `/abs/path`,
 * resolved once at *compile* time, fixed by where it's written, see
 * resolve_path_literal below), a `<name>` search path must resolve
 * *lazily*, at evaluation time: real Nix's own `findFile` internally
 * throws the exact same `ThrownError` class `throw` uses when nothing
 * matches (confirmed in cppnix's source), which is why `builtins.tryEval
 * (toString <nameNotInNIX_PATH>)` can catch it there -- resolving this
 * eagerly at compile time (the original, simpler design) would fire the
 * fatal before any `tryEval` could possibly be active yet, making it
 * uncatchable regardless of resolve_search_path's own tryEval-aware
 * fallback above. Pops nothing extra here; takes the name directly as a
 * Value (already pushed as an ordinary compile-time string constant by
 * compile_expr's AST_PATH case, exactly like a plain string literal). */
Value asnix_resolve_search_path_value(Value name_val) {
    int64_t len;
    const char *bytes = asnix_ptr_bytes(name_val, &len);
    if (len == 16 && memcmp(bytes, "nix/fetchurl.nix", 16) == 0) {
        const char *path = write_fetchurl_nix();
        return asnix_make_path_value(path, strlen(path));
    }
    char *resolved = canon_path(resolve_search_path(bytes, (size_t)len));
    Value v = asnix_make_path_value(resolved, strlen(resolved));
    free(resolved);
    return v;
}

/* M64: `~`-prefixed ("home") path resolution. Unlike an ordinary path
 * literal, real Nix's own HPATH parser action concatenates $HOME onto
 * the literal text *verbatim* -- NO canon_path call at all, no `.`/`..`/
 * doubled-slash collapsing (confirmed directly against `nix-instantiate
 * --eval`: numtide/nix-conformance's eval-okay-home-path-not-normalised.nix
 * is built specifically to probe this -- `~/a/../b` stays literally
 * "$HOME/a/../b", never collapsed to "$HOME/b"; the comment right at the
 * top of that fixture even names the reason: "parser.y's HPATH rule does
 * not call absPath"). `text`/`len` include the leading '~' (e.g.
 * "~/a/../b", or just "~/" for an interpolated path's first chunk --
 * lexer.c's try_scan_home_path guarantees `len >= 2` either way, since
 * HPATH always requires at least "~/"). Applies identically whether or
 * not interpolation follows afterward (real Nix's path_start: HPATH {...}
 * grammar action is the same either way; only the TEXT captured by the
 * token differs, not the formula -- see docs/architecture.md's M64
 * notes). */
static char *resolve_home_literal(const char *text, size_t len) {
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        fprintf(stderr, "asnix: cannot resolve '~' -- $HOME is not set\n");
        exit(1);
    }
    size_t home_len = strlen(home);
    size_t rest_len = len - 1; /* skip the leading '~' */
    char *joined = malloc(home_len + rest_len + 1);
    memcpy(joined, home, home_len);
    memcpy(joined + home_len, text + 1, rest_len);
    joined[home_len + rest_len] = '\0';
    return joined;
}

/* Resolves one ordinary (non-search, see asnix_resolve_search_path_value
 * above for that case) AST_PATH node's raw lexeme to an absolute,
 * canonicalized path string (malloc'd, NUL-terminated). M64: dispatches
 * to resolve_home_literal (no canon at all) for `~`-prefixed literals --
 * the trailing-slash-is-fatal check above that dispatch still applies to
 * both (real Nix's INPATH_SLASH "path has a trailing slash" error fires
 * the same way for a home path ending in '/' with nothing continuing it,
 * per its own lexer.l rule, which doesn't distinguish PATH from HPATH). */
static char *resolve_path_literal(Ast *node) {
    const char *text = node->as.path.text;
    size_t len = node->as.path.len;

    if (len > 0 && text[len - 1] == '/') {
        fprintf(stderr, "asnix: path has a trailing slash\n");
        exit(1);
    }

    if (node->as.path.is_home) {
        return resolve_home_literal(text, len);
    }

    char *joined;
    if (len > 0 && text[0] == '/') {
        joined = malloc(len + 1);
        memcpy(joined, text, len);
        joined[len] = '\0';
    } else {
        size_t base_len = strlen(g_compile_base_dir);
        joined = malloc(base_len + 1 + len + 1);
        memcpy(joined, g_compile_base_dir, base_len);
        joined[base_len] = '/';
        memcpy(joined + base_len + 1, text, len);
        joined[base_len + 1 + len] = '\0';
    }
    char *canon = canon_path(joined);
    free(joined);
    return canon;
}

/* M64: resolves the FIRST chunk of an interpolated path literal
 * (`./${foo}`'s "./", `~/${foo}`'s "~/", `/${foo}`'s "/") directly to an
 * absolute Path VALUE (not just a string -- this becomes parts[0] of an
 * OP_PATH_CONCAT chain, and that op's whole correctness rests on
 * parts[0] already being a genuine Path, see ir.h). Unlike
 * resolve_path_literal (a complete, standalone literal), a trailing '/'
 * here is completely ordinary -- it's exactly what lets the following
 * interpolated/literal parts glue on with the right separator (e.g.
 * "./${foo}"'s first chunk must resolve to this file's own directory
 * *with* a trailing slash, so concatenating "foo" after it reads
 * ".../foo" rather than ".../dirfoo") -- so it must be preserved, not
 * rejected. Mirrors real Nix's parser.y own path_start action exactly,
 * including its one subtlety: `canon_path` always strips a trailing
 * slash by construction, so it has to be re-added afterward, but only
 * when the ORIGINAL text both ended in '/' AND had more than that one
 * character -- a lone "/" (the whole of `/${foo}`'s first chunk) must
 * NOT gain a second slash (`CanonPath("/").abs()` is already just "/";
 * real Nix's own re-add condition is literally `literal.size() > 1 &&
 * literal.back() == '/'`, checked against `nix-instantiate --eval
 * '/${"foo"}'` -> "/foo", not "//foo"). The `~` branch reuses
 * resolve_home_literal verbatim -- no canon, no re-add logic needed at
 * all, since it's a plain concatenation that already preserves whatever
 * trailing characters the original text had. */
static Value resolve_path_chunk(const char *text, size_t len, int is_home) {
    if (is_home) {
        char *joined = resolve_home_literal(text, len);
        Value v = asnix_make_path_value(joined, strlen(joined));
        free(joined);
        return v;
    }
    int had_trailing_slash = (len > 1 && text[len - 1] == '/');
    char *joined;
    if (len > 0 && text[0] == '/') {
        joined = malloc(len + 1);
        memcpy(joined, text, len);
        joined[len] = '\0';
    } else {
        size_t base_len = strlen(g_compile_base_dir);
        joined = malloc(base_len + 1 + len + 1);
        memcpy(joined, g_compile_base_dir, base_len);
        joined[base_len] = '/';
        memcpy(joined + base_len + 1, text, len);
        joined[base_len + 1 + len] = '\0';
    }
    char *canon = canon_path(joined);
    free(joined);
    if (had_trailing_slash) {
        size_t clen = strlen(canon);
        char *with_slash = malloc(clen + 2);
        memcpy(with_slash, canon, clen);
        with_slash[clen] = '/';
        with_slash[clen + 1] = '\0';
        free(canon);
        canon = with_slash;
    }
    Value v = asnix_make_path_value(canon, strlen(canon));
    free(canon);
    return v;
}

/* One compile-time scope level == one runtime Env frame: `count` names,
 * each living at its own slot (De Bruijn depth = how many ScopeFrame.up
 * links away; slot = index within `names`/`name_lens`). AST_LAMBDA and the
 * root `builtins` frame just happen to use count==1 for the plain-
 * identifier case; AST_LET, `rec {}`, and a lambda-pattern's destructured
 * fields use count==(number of names) so every binding in one block
 * shares a single frame -- the structural piece that makes them mutually
 * visible (see compiler.c's AST_LET/AST_ATTRSET/AST_LAMBDA cases and
 * ir.h's OP_FRAME_* ops).
 *
 * `with` (see the AST_WITH case below) also introduces a ScopeFrame -- one
 * new runtime Env frame per `with`, exactly like let/lambda -- but with
 * `count == 0` (no statically-known names at all) and `is_with` set: it
 * never matches a direct lexical lookup, yet still has to count as one
 * more hop for every *other* name's depth, since depth is "how many
 * runtime Env frames up" and a with-frame really is one. `is_with` is what
 * lets AST_VAR's walk notice it and fall back to a bounded chain of
 * runtime lookups against each enclosing with-value, innermost first --
 * see the AST_VAR case. `count == 0` alone is NOT enough to mean "with
 * frame": the dummy `skip` level AST_LET builds for `inherit`'s outer-
 * scope resolution (see that case) is also zero-count but must NOT join
 * the with-fallback chain, hence the separate flag. */
typedef struct ScopeFrame {
    struct ScopeFrame *up;
    const char **names;
    size_t *name_lens;
    size_t count;
    int is_with;
    /* M-skip-cell: set only for a `rec {}`'s own frame when it has an
     * `__overrides` binding (see that AST_ATTRSET case's own comment).
     * `try_cell_ref` below refuses its Cell-aliasing optimization for
     * any name that resolves into such a frame -- `__overrides` rewrites
     * an already-populated sibling slot to a DIFFERENT object after the
     * fact (ib_rec_overrides_merge), which a Cell still gets right (it
     * re-chases the slot's CURRENT content every time), but an eager
     * "read the slot now, it's already non-empty" fast path at the op
     * level would bake in the pre-override value instead. Matches
     * iets's own fix for this exact case (eval/mod.rs fill_rec_slots's
     * `overrides: bool` parameter, forcing a real deferred thunk for
     * every binding in such a set rather than its own maybe_thunk fast
     * path) -- confirmed by reading that code directly. */
    int has_overrides;
} ScopeFrame;

/* Bounds how many enclosing `with`s a single unresolved-name fallback
 * chain can walk through -- real Nix has no such limit, but nesting this
 * deep never happens in practice and a fixed-size stack array keeps the
 * AST_VAR case simple (same spirit as other deliberate, documented scope
 * cuts in this project, e.g. M6's non-recursive `let`). */
#define ASNIX_MAX_WITH_CHAIN 64

/* perf: profiled directly (callgrind on real nixpkgs ffmpeg-full/plasma
 * evals) as the dominant self-cost inside compile_expr's own body -- the
 * AST_VAR/try_cell_ref scope-chain walk (compiler.c's `scope_find_lexical`
 * and AST_VAR's own copy of the same loop) calls this on average ~16
 * times per enclosing frame actually walked, ~357,000 frame-visits on the
 * real ffmpeg-full eval alone. A first real `rec {}` scope this project's
 * own M142 comment already measured at thousands of bindings (nixpkgs's
 * all-packages.nix) was the obvious first suspect for an O(log n)
 * bsearch-over-pre-sorted-symbol-ids rewrite (reusing AST_ATTRSET's
 * already-computed `sorted_syms`/`order`) -- tried, built, verified
 * correct (44/44 difftest, 148/148 nixpkgs-hash), but MEASURED a net
 * REGRESSION on real ffmpeg-full (+18.8M of 5.77B instructions, +0.33%):
 * direct instrumentation showed that path's own bsearch fires only ~2,100
 * times against ~357,000 ordinary small-frame linear-scan visits
 * (average 16.25 slots/frame) -- the rare big-frame win doesn't come
 * close to paying for the extra branch+lazy-intern cost paid on every
 * other (small, common) frame. Reverted in favor of this instead: almost
 * every one of those ~5.8M comparisons across the many small frames is a
 * MISMATCH (wrong sibling name), and a mismatch's first byte differs from
 * the target's far more often than not for arbitrary Nix identifiers --
 * rejecting on that alone, before ever calling memcmp, skips the
 * libc-call overhead for the overwhelming majority of comparisons,
 * without touching ScopeFrame, AST_VAR, or any other call site at all.
 *
 * Considered, built, and REJECTED: a real 128-bit SSE/AVX load instead
 * of memcmp for the common <=8/<=16-byte case (real Nix identifiers are
 * almost all this short). Two separate problems, confirmed by direct
 * measurement, not assumption: (1) unsafe as a literal wide load --
 * `name`/`a`/`b` here point into the source buffer `eval_file.c`'s
 * `read_file` allocates via `malloc(size + 1)` (padding for exactly one
 * NUL byte, nothing more), so an unaligned 16-byte `_mm_loadu_si128` for
 * an identifier in the file's last ~15 bytes would read past that
 * allocation -- a real heap-buffer-overflow. (2) even the SAFE version
 * (two bounded `memcpy`s into a zeroed `uint64_t` + one 8-byte compare,
 * never reading past `alen`) measured as a NET REGRESSION on real
 * ffmpeg-full (+0.21% instructions, confirmed via callgrind) versus this
 * 3-line version: the extra branch/locals pushed `name_eq` past the
 * compiler's inline-size threshold, so what used to be folded into every
 * caller for free (confirmed: this function has zero self-cost of its
 * own in the profile, below) became a real CALL at ~380,000 call sites
 * -- that overhead alone outweighed the memcmp calls it avoided. Also:
 * glibc's own `memcmp` already uses AVX2 on this machine for longer runs
 * (confirmed in the profile: `__memcmp_avx2_movbe`) -- the cost this
 * function's short-name case was paying was the call + internal
 * length-dispatch, not a lack of vectorization, so there was no missing
 * SIMD to add in the first place. */
static int name_eq(const char *a, size_t alen, const char *b, size_t blen) {
    if (a == NULL || alen != blen) return 0;
    if (alen == 0) return 1;
    if (a[0] != b[0]) return 0;
    return memcmp(a, b, alen) == 0;
}

/* M92: factored out of AST_VAR's own scope walk below -- scans `scope`
 * outward for a LEXICAL match of `name`/`name_len` (ignoring `with`
 * fallback entirely: a lexical binding always wins over any enclosing
 * `with`, even one introduced further out than the match, so finding one
 * here is the WHOLE answer regardless of how many with-frames were
 * crossed on the way -- see AST_VAR's own comment for why that's
 * correct). Returns 1 and writes `*out_depth`/`*out_slot` on a match, 0
 * (leaving them untouched) if `name` isn't bound lexically anywhere in
 * `scope` -- the caller is then on its own for a `with`-fallback chain or
 * an "undefined variable" error, exactly as AST_VAR already was before
 * this refactor. Also the engine behind `try_cell_ref` below: a name that
 * resolves lexically has a fixed, stable compile-time (depth, slot) --
 * exactly what a Cell needs to alias a single Env slot forever; a name
 * that only resolves via `with` does not (which `with`, if any, actually
 * has the key is a runtime fact), so `try_cell_ref` simply never fires
 * for one -- the "with-sourced names never alias" exclusion the M92
 * design calls for falls out of this function returning 0, with no extra
 * special-casing needed. */
/* `out_has_overrides` may be NULL for a caller that doesn't care (e.g.
 * the native-builtin-call-fusion check below, which only wants depth/
 * slot) -- written only on a match, same as out_depth/out_slot. */
static int scope_find_lexical(ScopeFrame *scope, const char *name, size_t name_len, int *out_depth,
                               int64_t *out_slot, int *out_has_overrides) {
    int depth = 0;
    for (ScopeFrame *f = scope; f; f = f->up, depth++) {
        for (size_t slot = 0; slot < f->count; slot++) {
            if (name_eq(f->names[slot], f->name_lens[slot], name, name_len)) {
                *out_depth = depth;
                *out_slot = (int64_t)slot;
                if (out_has_overrides) *out_has_overrides = f->has_overrides;
                return 1;
            }
        }
    }
    return 0;
}

/* M92: the `maybeThunk`-optimization check -- called at each of the ten
 * `ib_mk_thunk` call sites (list elements, call arguments, let/attrs
 * bindings, the with-expr itself) in place of the default "wrap it in a
 * fresh OP_MK_THUNK" path. Mirrors real Nix's ONE `Expr::maybeThunk`
 * override, `ExprVar::maybeThunk` (~/nix/gc-unlock/src/libexpr/eval.cc,
 * ~line 1048-1065): a bare variable reference that resolves lexically
 * skips allocating a fresh deferred-computation cell entirely and instead
 * emits OP_VAR_REF_CELL, aliasing the EXISTING Env slot that variable
 * already lives in (see heap.h's ASNIX_CELL_MARKER comment for why this
 * matters -- it's what lets `==`/the printer recover real Nix's
 * `&v1==&v2` slot-identity shortcut). Returns 1 if it emitted the Cell op
 * (the caller must NOT also emit the ordinary thunk in that case), 0
 * otherwise -- which happens for any non-AST_VAR expression, AND for a
 * bare AST_VAR that only resolves via a `with` fallback (no fixed slot to
 * alias; see scope_find_lexical's own comment) -- the caller falls back
 * to the `is_thunk_free_literal`/ordinary-`ib_mk_thunk` path unchanged in
 * either case. Checked BEFORE `is_thunk_free_literal` at every call site:
 * the two predicates are mutually exclusive by AST kind (this one only
 * ever fires for AST_VAR; that one explicitly excludes it, by its own
 * header comment), so the check order doesn't change behavior, but
 * `try_cell_ref` is the cheaper/more specific test. */
static int try_cell_ref(IrBuilder *b, Ast *node, ScopeFrame *scope) {
    if (node->kind != AST_VAR) return 0;
    int depth;
    int64_t slot;
    int has_overrides = 0;
    if (!scope_find_lexical(scope, node->as.var.name, node->as.var.name_len, &depth, &slot, &has_overrides))
        return 0;
    /* M-skip-cell: a name resolving into a `rec{}` frame with its own
     * `__overrides` binding must not alias through this optimization --
     * see ScopeFrame's own has_overrides comment. Falls back to the
     * caller's ordinary ib_mk_thunk path, which still re-resolves this
     * same (depth, slot) fresh at force time via a real deferred thunk,
     * exactly like the unconditional pre-this-change behavior. */
    if (has_overrides) return 0;
    ib_var_ref_cell(b, depth, slot);
    return 1;
}

/* Insertion sort by symbol id -- every call site sorts a source-level
 * formal/binding count, always tiny. */
static void sort_syms(uint32_t *syms, size_t n) {
    for (size_t i = 1; i < n; i++) {
        size_t j = i;
        while (j > 0 && syms[j - 1] > syms[j]) {
            uint32_t tmp = syms[j - 1];
            syms[j - 1] = syms[j];
            syms[j] = tmp;
            j--;
        }
    }
}

/* perf (M142): AST_ATTRSET's own binding-sort (unlike sort_syms above,
 * whose call sites really are tiny lambda-formals counts) cannot assume
 * "always tiny" -- a single top-level rec {} literal in real nixpkgs
 * (pkgs/top-level/all-packages.nix) has thousands of bindings. qsort
 * gives O(n log n) instead of insertion sort's O(n^2). */
typedef struct {
    uint32_t sym;
    size_t idx;
} SymIdxPair;

/* perf (M183): bottom-up merge sort over `.sym` alone, specialized for
 * `SymIdxPair` -- same fix, same rationale, as builtins_shared.c's
 * sibling `merge_sort_make_attrs_entries` (not shared via a header since
 * the two sorts operate on unrelated data, matching this file's own
 * existing convention for `ASNIX_ATTRSET_SORT_INSERTION_MAX`/
 * `ASNIX_MAKE_ATTRS_SORT_INSERTION_MAX`). Removes the indirect call
 * through `symidxpair_cmp` that `qsort` paid once per comparison. Names
 * are already unique by the time this runs (this case's own comment,
 * below) so there is no tie-break to preserve either way -- any stable
 * sort (this one included: ties, if they ever occurred, resolve to the
 * left run) reproduces `symidxpair_cmp`'s behavior exactly. */
static SymIdxPair *merge_sort_symidxpairs(SymIdxPair *a, SymIdxPair *tmp, size_t n) {
    SymIdxPair *src = a, *dst = tmp;
    for (size_t width = 1; width < n; width *= 2) {
        for (size_t i = 0; i < n; i += 2 * width) {
            size_t mid = i + width < n ? i + width : n;
            size_t end = i + 2 * width < n ? i + 2 * width : n;
            size_t l = i, r = mid, k = i;
            while (l < mid && r < end) {
                if (src[l].sym <= src[r].sym) dst[k++] = src[l++];
                else dst[k++] = src[r++];
            }
            while (l < mid) dst[k++] = src[l++];
            while (r < end) dst[k++] = src[r++];
        }
        SymIdxPair *t = src; src = dst; dst = t;
    }
    return src;
}
/* M146: compile_expr's AST_LET/AST_LAMBDA(pattern)/AST_ATTRSET cases each
 * malloc a handful of small scratch arrays -- sized by a purely
 * SOURCE-LEVEL count (`let` binding count, lambda-pattern formal count,
 * attrset literal binding count) -- fill them, use them for the span of
 * one compile_expr call, and free them again before returning. Real
 * nixpkgs compiles ~1057 imported files' worth of source for one `d.nix`
 * eval, and the overwhelming majority of these literals have a handful
 * of bindings/formals (a `{ lib, stdenv, fetchurl, ... }:` lambda pattern,
 * a small `let x = ...; in`, a package's attrset) -- a profiling pass
 * (instruction-sampled call-graph, M146) found `compile_expr`'s own
 * recursive descent responsible for a large, direct share of asnix's
 * malloc-family instruction cost, ahead of any single runtime call site.
 * A fixed-size stack array below this cap, used whenever the real count
 * fits, removes the malloc/free pair entirely for that overwhelmingly
 * common case; a literal with MORE bindings/formals than the cap (rare --
 * the known outlier, pkgs/top-level/all-packages.nix's ~4,280-binding
 * attrset, already takes a dedicated large-n path for its OWN sort
 * algorithm below) falls back to the original malloc path unchanged. This
 * is NOT the depth-indexed pool M144 used for derivation.c's runtime
 * scratch arrays -- compile_expr's own recursion is plain, single-
 * threaded C call-stack recursion with no cross-call reuse opportunity
 * (each node is compiled exactly once), so a plain C-local array --
 * fresh, and automatically distinct, on every recursive activation -- is
 * simpler and sufficient; no pooling/indexing scheme is needed at all.
 * Deliberately the SAME constant as the insertion-sort-vs-qsort threshold
 * right below (`#define`d FROM it, not just equal by coincidence) so the
 * two decisions -- "is this literal small enough to sort with plain
 * insertion sort" and "is this literal small enough to put its scratch on
 * the stack" -- can never drift apart. */
#define ASNIX_COMPILE_SCRATCH_MAX 32
/* Threshold below which plain insertion sort beats qsort's per-comparison
 * indirect-call overhead -- tuned empirically against the real d.nix eval,
 * see the AST_ATTRSET case's own comment. */
#define ASNIX_ATTRSET_SORT_INSERTION_MAX ASNIX_COMPILE_SCRATCH_MAX
/* Factored out of compile_expr's AST_ATTRSET case so the (astronomically
 * rare -- only literals bigger than the threshold above ever reach this)
 * qsort path doesn't grow compile_expr's own code size/layout for the
 * common case it's never taken in -- an earlier version inlined this
 * directly into the switch and measured a small but real regression on
 * the COMMON (small-n) path purely from compile_expr growing larger. */
static void sort_attrset_bindings_large(uint32_t *syms, size_t *order, size_t n) {
    SymIdxPair *pairs = asnix_arena_alloc(n * sizeof(SymIdxPair));
    SymIdxPair *scratch = asnix_arena_alloc(n * sizeof(SymIdxPair));
    for (size_t i = 0; i < n; i++) { pairs[i].sym = syms[i]; pairs[i].idx = i; }
    SymIdxPair *sorted = merge_sort_symidxpairs(pairs, scratch, n);
    for (size_t i = 0; i < n; i++) order[i] = sorted[i].idx;
}

/* perf: these expression kinds already compile to a single zero-cost
 * compile-time push (ib_push_int/true/false/null, or ib_push_const for a
 * pooled constant) -- no captured scope, no deferred work, no runtime
 * op beyond the push itself. Same reasoning as real Nix's own
 * `maybeThunk` overrides for ExprInt/ExprFloat/ExprString/ExprPath
 * (~/nix/gc-unlock/src/libexpr/eval.cc): wrapping one of these in a
 * Thunk (a 32-byte heap allocation plus a whole separate child
 * IrProgram, forced through the full OP_CALL/force machinery at every
 * point of use) is pure overhead for a value that costs nothing to
 * produce and never benefits from being deferred. Every call site that
 * thunks a list element / binding value / call argument already
 * tolerates either an already-forced Value or a Thunk there (that's
 * what `force` means), so substituting the immediate compile_expr()
 * output for a thunk wrapping the identical value is observably
 * identical, just cheaper -- not applicable to AST_VAR (a bare variable
 * reference): that's a distinct optimization (real Nix's own
 * `ExprVar::maybeThunk` slot-aliasing), out of scope here. */
/* M136: compile-time constant folding for `AST_BINOP` when both operands
 * are int literals -- mirrors op_arith.S's own overflow-checked semantics
 * EXACTLY (both the true int64-overflow check via `__builtin_*_overflow`
 * AND the narrower ASNIX_INT_MIN/MAX tagged-value window `+`/`-`/`*` are
 * additionally bounded to there, see op_arith.S's own M56 comment) --
 * any case that would abort at runtime (overflow, division by zero, the
 * `ASNIX_INT_MIN / -1` edge) is deliberately left unfolded, so the
 * ordinary runtime op still reaches the exact same abort with the exact
 * same message, never silently wrapping or producing a different result
 * than an unoptimized build would. `>`/`<=` (`'g'`/`'L'`) compute the
 * same boolean real Nix's own operand-swap desugaring produces (see this
 * file's AST_BINOP case's own M51 comment for why the swap exists in the
 * *compiled* form) directly from the source-level operands here, since
 * this function never emits IR, only a final bool/int/float. `//`/`++`
 * (`'u'`/`'c'`) fall through to `default` and are never folded: those
 * operators are for attrsets/lists, so two numeric literals on either
 * side is always a runtime type error this function must not mask by
 * computing a bogus int/bool/float result for it. Called both from the
 * AST_BINOP case of `compile_expr` below (to actually fold) and from
 * `is_thunk_free_literal` immediately below (so a foldable binop is ALSO
 * recognized as thunk-free, exactly like any other literal -- M92's
 * `maybeThunk` reasoning applies identically once this function has
 * reduced the node to a single zero-cost push).
 *
 * <follow-on to M136>: extended to AST_FLOAT operands (and mixed int/
 * float pairs), the first of M136's own two explicitly-scoped-out
 * follow-ons. Int/int (both operands AST_INT) keeps the EXACT path
 * above unchanged -- a pure-int fold must keep its exact int64 overflow
 * semantics, not get silently rerouted through double arithmetic (which
 * would both lose precision for large ints near ASNIX_INT_MAX/MIN and
 * change the *type* of the result from int to float, a real behavior
 * change real Nix does not make for `1 + 2`). The float branch only
 * runs once at least one operand is AST_FLOAT, and mirrors value_util.c's
 * own int/float-promotion rules exactly: an int operand casts to double
 * the identical way `as_double()` does there (so a literal fold is
 * bit-for-bit identical to what the corresponding unfolded runtime op
 * would have computed, not merely "close"); `+`/`-`/`*` never fail to
 * fold (real IEEE-754 double arithmetic has no overflow-abort case the
 * way checked-int arithmetic does -- a too-large result is `inf`, the
 * exact same `inf` the unfolded op would also produce, see
 * asnix_add/sub/mul_values, none of which ever call asnix_fatal); `/`
 * is left unfolded exactly when the (promoted-to-double) divisor is
 * 0.0, mirroring asnix_div_values's own unconditional "divisor == 0.0"
 * fatal check (real Nix's own rule: EVERY division by a zero divisor is
 * a hard error, even `1.0 / 0.0`, never silently IEEE-754 infinity --
 * see that function's own comment) so the runtime op still reaches the
 * identical "division by zero" abort. The six comparisons compute a
 * plain double `<`/`<=`/`>`/`>=`/`==`/`!=`, the same promotion
 * asnix_cmp_{lt,gt,le,ge}_values / asnix_bshared_value_eq's own
 * Float-on-either-side branch already use at runtime. */
static int try_fold_const_binop(Ast *node, int *out_is_bool, int *out_is_float, int64_t *out_val,
                                 double *out_fval) {
    if (node->kind != AST_BINOP) return 0;
    Ast *lhs = node->as.binop.lhs, *rhs = node->as.binop.rhs;
    int lhs_is_int = (lhs->kind == AST_INT), rhs_is_int = (rhs->kind == AST_INT);
    int lhs_is_float = (lhs->kind == AST_FLOAT), rhs_is_float = (rhs->kind == AST_FLOAT);
    if (!(lhs_is_int || lhs_is_float) || !(rhs_is_int || rhs_is_float)) return 0;

    if (lhs_is_int && rhs_is_int) {
        int64_t a = lhs->as.int_val, b = rhs->as.int_val, result;
        switch (node->as.binop.op) {
            case '+':
                if (__builtin_add_overflow(a, b, &result)) return 0;
                break;
            case '-':
                if (__builtin_sub_overflow(a, b, &result)) return 0;
                break;
            case '*':
                if (__builtin_mul_overflow(a, b, &result)) return 0;
                break;
            case '/':
                if (b == 0) return 0;
                if (a == INT64_MIN && b == -1) return 0;
                result = a / b;
                break;
            case 'e': *out_is_bool = 1; *out_is_float = 0; *out_val = (a == b); return 1;
            case 'n': *out_is_bool = 1; *out_is_float = 0; *out_val = (a != b); return 1;
            case 'l': *out_is_bool = 1; *out_is_float = 0; *out_val = (a < b); return 1;
            case 'G': *out_is_bool = 1; *out_is_float = 0; *out_val = (a >= b); return 1;
            case 'g': *out_is_bool = 1; *out_is_float = 0; *out_val = (a > b); return 1;
            case 'L': *out_is_bool = 1; *out_is_float = 0; *out_val = (a <= b); return 1;
            default: return 0;
        }
        if (result < ASNIX_INT_MIN || result > ASNIX_INT_MAX) return 0;
        *out_is_bool = 0;
        *out_is_float = 0;
        *out_val = result;
        return 1;
    }

    /* At least one operand is AST_FLOAT -- promote both to double exactly
     * like as_double() does (an int operand's cast is the identical C
     * `(double)` cast, so this is bit-for-bit what the unfolded runtime
     * op would have computed from the same two literals). */
    double a = lhs_is_float ? lhs->as.float_val : (double)lhs->as.int_val;
    double b = rhs_is_float ? rhs->as.float_val : (double)rhs->as.int_val;
    switch (node->as.binop.op) {
        case '+': *out_is_bool = 0; *out_is_float = 1; *out_fval = a + b; return 1;
        case '-': *out_is_bool = 0; *out_is_float = 1; *out_fval = a - b; return 1;
        case '*': *out_is_bool = 0; *out_is_float = 1; *out_fval = a * b; return 1;
        case '/':
            if (b == 0.0) return 0; /* matches asnix_div_values's unconditional divisor-==0.0 fatal */
            *out_is_bool = 0;
            *out_is_float = 1;
            *out_fval = a / b;
            return 1;
        case 'e': *out_is_bool = 1; *out_is_float = 0; *out_val = (a == b); return 1;
        case 'n': *out_is_bool = 1; *out_is_float = 0; *out_val = (a != b); return 1;
        case 'l': *out_is_bool = 1; *out_is_float = 0; *out_val = (a < b); return 1;
        case 'G': *out_is_bool = 1; *out_is_float = 0; *out_val = (a >= b); return 1;
        case 'g': *out_is_bool = 1; *out_is_float = 0; *out_val = (a > b); return 1;
        case 'L': *out_is_bool = 1; *out_is_float = 0; *out_val = (a <= b); return 1;
        default: return 0;
    }
}

static int is_thunk_free_literal(Ast *node) {
    switch (node->kind) {
        case AST_INT:
            /* M-range-fix: an out-of-range int literal must NOT be
             * eagerly inlined here -- unlike every other case in this
             * function, pushing it has an observable side effect
             * (ib_push_int's own runtime range check fatals). Inlining
             * it would force a value real Nix's laziness would never
             * touch if the enclosing binding is never read (e.g. a
             * `let maxHandle = 9223372036854775806; in ...` used only
             * inside a disabled `lib.mkIf false { ... }` branch -- found
             * in nixos/modules/services/network-filesystems/orangefs/
             * server.nix). Falling through to the ordinary OP_MK_THUNK
             * path keeps it lazy, matching try_fold_const_binop's own
             * ASNIX_INT_MIN/MAX guard (below) for the folded-binop case. */
            return node->as.int_val >= ASNIX_INT_MIN && node->as.int_val <= ASNIX_INT_MAX;
        case AST_FLOAT:
        case AST_BOOL:
        case AST_NULL:
        case AST_CURPOS:
            return 1;
        case AST_BINOP: {
            int is_bool, is_float;
            int64_t val;
            double fval;
            return try_fold_const_binop(node, &is_bool, &is_float, &val, &fval);
        }
        case AST_STRING:
            for (size_t i = 0; i < node->as.string.count; i++) {
                if (node->as.string.parts[i].is_expr) return 0;
            }
            return 1;
        case AST_PATH:
            return !node->as.path.is_search && node->as.path.interp_count == 0;
        case AST_LIST:
            /* M101-era find: real Nix's own `ExprList::maybeThunk`
             * (~/nix/gc-unlock/src/libexpr/eval.cc) special-cases ONLY
             * the empty list -- it returns a shared singleton Value,
             * never allocating a Thunk for `[]` wherever it appears as a
             * list element/binding value/call argument/etc. A NON-empty
             * list falls through to an ordinary thunk there, because
             * each element still captures the enclosing scope (genuine
             * deferred work: `let x = [(abort "x")]; in 5` must not run
             * the element's code merely by resolving `x`'s binding slot,
             * same invariant M53's own per-element thunking already
             * protects). The empty case has no elements to capture and
             * compiles to a single zero-cost `ib_mk_list(b, 0)` (see the
             * AST_LIST case in compile_expr below) -- same "no captured
             * scope, no deferred work" criteria as every other case in
             * this switch. */
            return node->as.list.count == 0;
        default:
            return 0;
    }
}

static void compile_expr(IrBuilder *b, Ast *node, ScopeFrame *scope);

/* M110: table-driven generalization of a prior session's single-case
 * `builtins.elem a b` fusion proof-of-concept (see docs/architecture.md's
 * own M110 entry for the full story, and that prior attempt's commit for
 * the original single-native version). Measured directly on `d.nix`'s
 * real ffmpeg closure, via a gdb breakpoint-hit-count pass over all ~32
 * curried natives' own `*_impl` functions (not guessed): `all`/`any`/
 * `match`/`elem`/`intersectAttrs` are, in that order, the five highest-
 * volume curried-native calls by a wide margin (18151/12015/9835/7825/
 * 7763 hits respectively, next-highest `compareVersions` at 2715) --
 * together they account for the large majority of all curried-native
 * calls observed. All five are arity-2.
 *
 * Each table entry's `noforce` flag must match the native's own build_*
 * registration EXACTLY (`asnix_bshared_make_curried_native_noforce` vs.
 * the ordinary `asnix_bshared_make_curried_native`) -- this is the one
 * real risk surface a wrong fusion could introduce (a silently wrong
 * VALUE, not a crash), not the AST-shape recognition itself:
 *
 * - `elem` (NOFORCE): its own curry chain reassembles both arguments
 *   with `ib_var_ref_raw` (no forcing) and its native impl
 *   (asnix_prim_elem2_impl) forces ONLY `list` (args[1]) itself, leaving
 *   `x` (args[0]) deliberately unforced -- see that function's own M92
 *   comment for why (`eqValues`-style slot-identity preservation). The
 *   fused call site below compiles both arguments exactly like any other
 *   ordinary call argument (the same try_cell_ref/is_thunk_free_literal/
 *   thunk fallback every AST_APP case already uses) and mints the primop
 *   via `asnix_bshared_make_primop_noforce` -- op_call.S's own dispatch
 *   (see its M25/M30 comments) then skips its post-dispatch force loop
 *   for a NOFORCE primop, so both arguments reach the native function
 *   exactly as unforced as the original 2-hop chain would have delivered
 *   them. Reusing this exact mechanism for a 3rd/4th/5th noforce native
 *   would need the SAME audit of what, if anything, that native forces
 *   manually -- not attempted here beyond `elem`, which was already
 *   proven correct by the prior session's work.
 *
 * - `any`/`all`/`match`/`intersectAttrs` (ordinary force-by-default):
 *   their own curry chains reassemble arguments with plain `ib_var_ref`
 *   (forces on read) before the tail call into an ordinary (non-NOFORCE)
 *   primop -- op_call.S's post-dispatch force loop then forces them
 *   AGAIN there, redundantly but cheaply (asnix_force_for_print's fast
 *   path on an already-forced Value is a single tag check). The fused
 *   call site compiles both arguments the identical ordinary way (same
 *   thunk-wrap fallback) and mints an ordinary (force) primop via
 *   `asnix_bshared_make_primop` -- op_call.S's SAME force loop then does
 *   the forcing exactly once, landing in the exact same already-forced
 *   state the native impl always required. No native-impl-specific
 *   handling needed at all for this group: the generic OP_CALL dispatch
 *   already provides it, which is *why* these four were mechanically
 *   simpler to add correctly than `elem`'s own NOFORCE case, not harder.
 *
 * Every other curried native (`foldl'`/`replaceStrings`/`attrByPath`,
 * arity-3; `add`/`sub`/`mul`/`div`/`bitAnd`/`bitOr`/`bitXor`/
 * `compareVersions`/`concatMap`/`genList`/`sort`/`partition`/`catAttrs`/
 * `split`/`seq`/`deepSeq`/`hashFile`/`groupBy`/`zipAttrsWith`/
 * `appendContext`/`scopedImport`/`unsafeGetAttrPos`/`filterAttrs`/
 * `filterSource`) is deliberately left out of this table: the same gdb
 * measurement found each of them individually orders of magnitude rarer
 * on this real workload than the five included here (the very next
 * busiest, `compareVersions`, at 2715 hits vs. `intersectAttrs`'s 7763),
 * so extending further buys steadily less for the same per-native
 * forcing-semantics audit cost -- not a correctness concern, just not
 * where the measured volume is. */
typedef struct {
    const char *name;
    size_t name_len;
    void *fn;
    int noforce;
} FusedNative2;

static const FusedNative2 FUSED_NATIVES_2[] = {
    { "elem", 4, (void *)asnix_prim_elem2_impl, 1 },
    { "any", 3, (void *)asnix_prim_any2_impl, 0 },
    { "all", 3, (void *)asnix_prim_all2_impl, 0 },
    { "match", 5, (void *)asnix_prim_match2_impl, 0 },
    { "intersectAttrs", 15, (void *)asnix_prim_intersectAttrs2_impl, 0 },
};
#define FUSED_NATIVES_2_COUNT (sizeof(FUSED_NATIVES_2) / sizeof(FUSED_NATIVES_2[0]))

/* Recognizes the literal 2-deep application shape `builtins.<name> a b`
 * where `<name>` is one of FUSED_NATIVES_2 above, `builtins` resolves
 * lexically to the genuine root ScopeFrame (not shadowed by a user's own
 * `let builtins = ...;`), and the select itself is static (no `.${...}`,
 * no `or` default) -- see the big comment above for the per-native
 * forcing-semantics rationale, and the original single-native
 * (`elem`-only) proof-of-concept this generalizes for the root-frame-
 * shadow-check rationale (identical here, reusing the same
 * scope_find_lexical + `.up == NULL` walk M92 established). On a match,
 * compiles a direct N-arg native call -- skipping the curried native's
 * own Closure/Env hops entirely -- instead of falling through to the
 * ordinary AST_APP compilation of a (by now fully-collected) call. */
static int try_fused_native_call2(IrBuilder *b, Ast *node, ScopeFrame *scope) {
    if (node->kind != AST_APP) return 0;
    Ast *inner = node->as.app.fn;
    if (inner->kind != AST_APP) return 0;
    Ast *sel = inner->as.app.fn;
    /* All ~32 "curried native" builtins (elem, any, all, intersectAttrs,
     * match, etc.) are reachable ONLY as `builtins.<name>` -- a bare
     * `elem 1 2` with no local binding is "undefined variable" in this
     * codebase (confirmed directly by a prior session's work) -- so the
     * shape to recognize is AST_SELECT(AST_VAR("builtins"), name), not a
     * bare AST_VAR. */
    if (sel->kind != AST_SELECT) return 0;
    if (sel->as.select.attr_expr != NULL) return 0; /* dynamic .${expr} -- not statically known */
    if (sel->as.select.default_ != NULL) return 0;  /* `or` form -- different op entirely, skip */
    Ast *builtins_var = sel->as.select.target;
    if (builtins_var->kind != AST_VAR) return 0;
    if (!name_eq(builtins_var->as.var.name, builtins_var->as.var.name_len, "builtins", 8)) return 0;

    const FusedNative2 *entry = NULL;
    for (size_t i = 0; i < FUSED_NATIVES_2_COUNT; i++) {
        if (name_eq(sel->as.select.attr, sel->as.select.attr_len, FUSED_NATIVES_2[i].name,
                    FUSED_NATIVES_2[i].name_len)) {
            entry = &FUSED_NATIVES_2[i];
            break;
        }
    }
    if (entry == NULL) return 0;

    int depth;
    int64_t slot;
    if (!scope_find_lexical(scope, builtins_var->as.var.name, builtins_var->as.var.name_len, &depth, &slot, NULL))
        return 0;
    ScopeFrame *f = scope;
    for (int i = 0; i < depth; i++) f = f->up;
    if (f->up != NULL) return 0; /* `builtins` itself shadowed by a real local binding -- don't fuse */

    Ast *arg1 = inner->as.app.arg;
    Ast *arg2 = node->as.app.arg;
    /* Callee pushed FIRST, matching every other call site's own stack
     * convention (compile_expr(fn) always runs before the argument is
     * compiled -- see AST_APP's own code below) -- OP_CALL expects the
     * callee beneath its arguments on VALSTACK, not above them. */
    Value native_primop = entry->noforce ? asnix_bshared_make_primop_noforce(entry->fn, 2)
                                          : asnix_bshared_make_primop(entry->fn, 2);
    ib_push_const(b, (int64_t)native_primop.raw);
    if (!try_cell_ref(b, arg1, scope)) {
        if (is_thunk_free_literal(arg1)) {
            compile_expr(b, arg1, scope);
        } else {
            IrBuilder *arg_b = ib_new();
            compile_expr(arg_b, arg1, scope);
            ib_ret(arg_b);
            ib_mk_thunk(b, ib_finish(arg_b));
        }
    }
    if (!try_cell_ref(b, arg2, scope)) {
        if (is_thunk_free_literal(arg2)) {
            compile_expr(b, arg2, scope);
        } else {
            IrBuilder *arg_b = ib_new();
            compile_expr(arg_b, arg2, scope);
            ib_ret(arg_b);
            ib_mk_thunk(b, ib_finish(arg_b));
        }
    }
    ib_call(b, 2);
    return 1;
}

/* Factored out of AST_APP's own case below (same three-way branch,
 * unchanged): a call argument is call-by-need (see AST_APP's own long
 * comment, below, for why), compiled as a Cell-aliased bare variable
 * when possible, inlined directly when it's a thunk-free literal, or
 * wrapped in a fresh OP_MK_THUNK capturing the caller's env otherwise.
 * Shared between the ordinary single-argument path and the chained-
 * application path (OP_CALL_CHAIN, ir.h) so both compile each argument
 * identically. */
static void compile_app_arg(IrBuilder *b, Ast *arg, ScopeFrame *scope) {
    if (try_cell_ref(b, arg, scope)) {
        /* nothing more to do -- OP_VAR_REF_CELL already emitted */
    } else if (is_thunk_free_literal(arg)) {
        compile_expr(b, arg, scope);
    } else {
        IrBuilder *arg_b = ib_new();
        compile_expr(arg_b, arg, scope);
        ib_ret(arg_b);
        ib_mk_thunk(b, ib_finish(arg_b));
    }
}

static void compile_expr(IrBuilder *b, Ast *node, ScopeFrame *scope) {
    /* Debug-only (irbuild.h's ib_enable_line_tracking, off by default): a
     * plain field write, no branch, cheap enough to leave unconditional
     * rather than checking a flag on every single node compiled -- see
     * ib_set_current_src_pos's own comment. `node->src_pos` is 0 for any
     * node compiler.c itself synthesizes (desugared formals, etc. --
     * no Parser in scope there), which irdump prints as "line=?". */
    ib_set_current_src_pos(node->src_pos);
    switch (node->kind) {
        case AST_INT:
            ib_push_int(b, node->as.int_val);
            return;
        case AST_FLOAT: {
            /* M51: heap-allocated once, here, at compile time -- same
             * "resolve once, not per-eval" precedent AST_PATH's literal
             * resolution already established (compiler.c's AST_PATH
             * case): a Float is a genuine 2-word heap box (see heap.h),
             * not an immediate, so there's no cheaper way to materialize
             * one than allocating it, and a literal's value never
             * changes between evaluations of the same compiled program,
             * so allocating it fresh on every OP_PUSH_CONST hit (if this
             * were instead done at eval time) would be pure waste. Same
             * mechanism AST_PATH/AST_STRING already use for a heap-value
             * compile-time constant: ib_push_const stashes the raw
             * tagged word in the constant pool. */
            Value fv = asnix_make_float_value(node->as.float_val);
            ib_push_const(b, (int64_t)fv.raw);
            return;
        }
        case AST_BOOL:
            if (node->as.bool_val)
                ib_push_true(b);
            else
                ib_push_false(b);
            return;
        case AST_NULL:
            ib_push_null(b);
            return;
        case AST_VAR: {
            int depth = 0;
            int64_t with_depths[ASNIX_MAX_WITH_CHAIN];
            size_t with_count = 0;
            for (ScopeFrame *f = scope; f; f = f->up, depth++) {
                /* A lexical binding always wins over any enclosing `with`,
                 * even one introduced further out than this match (real
                 * Nix semantics: `with` is a fallback, never a shadow) --
                 * returning here before ever looking at with_depths is
                 * exactly that. */
                for (size_t slot = 0; slot < f->count; slot++) {
                    if (name_eq(f->names[slot], f->name_lens[slot], node->as.var.name,
                                node->as.var.name_len)) {
                        ib_var_ref(b, depth, (int64_t)slot);
                        return;
                    }
                }
                if (f->is_with) {
                    if (with_count >= ASNIX_MAX_WITH_CHAIN) {
                        fprintf(stderr, "asnix: too many nested `with` scopes (limit %d)\n",
                                ASNIX_MAX_WITH_CHAIN);
                        exit(1);
                    }
                    with_depths[with_count++] = depth;
                }
            }
            if (with_count == 0) {
                fprintf(stderr, "asnix: undefined variable '%.*s'\n", (int)node->as.var.name_len,
                        node->as.var.name);
                exit(1);
            }
            /* No lexical match anywhere -- fall back to a runtime search
             * over each enclosing `with`'s attrset, innermost first,
             * stopping at the first one that has the key (whether it
             * does is a runtime fact; the *chain itself*, built here, is
             * entirely compile-time since the with-nesting is static).
             * Each level: OP_VAR_REF reads+forces that with-frame's slot 0
             * (an ordinary lexical lookup -- the with-frame's depth is
             * already known), OP_WITH_TRY searches it and either falls
             * through with a match or jumps to the next level, and the
             * OP_JMP right after only runs on a hit, skipping straight to
             * `end` past every remaining level. If no level has the key,
             * execution falls into OP_WITH_MISS. */
            uint32_t sym = asnix_intern(node->as.var.name, node->as.var.name_len);
            size_t jmp_sites[ASNIX_MAX_WITH_CHAIN];
            for (size_t i = 0; i < with_count; i++) {
                ib_var_ref(b, with_depths[i], 0);
                size_t try_site = ib_with_try(b, sym);
                jmp_sites[i] = ib_jmp(b);
                size_t next_idx = ib_len(b);
                ib_patch_delta(b, try_site, next_idx);
            }
            ib_with_miss(b);
            size_t end_idx = ib_len(b);
            for (size_t i = 0; i < with_count; i++) ib_patch_delta(b, jmp_sites[i], end_idx);
            return;
        }
        case AST_LET: {
            /* Real Nix `let ... in` is implicitly `letrec`: every binding
             * is visible to itself and to every sibling binding in the
             * same block, regardless of source order. OP_FRAME_ALLOC
             * allocates this let's Env frame and installs it as ENV
             * *before* any binding's thunk is built, so each OP_MK_THUNK
             * (still lazy, same as before) naturally captures this frame
             * as its scope -- self/sibling references just resolve
             * through the frame like any other variable, once forced.
             * `inherit`ed bindings are the one exception: their value
             * must resolve against the scope *enclosing* this `let`, not
             * this new frame (see ast.h). It's NOT enough to just compile
             * them against `scope` directly, though: by the time any
             * binding's thunk is built (including the inherited one),
             * OP_FRAME_ALLOC has already switched the *runtime* ENV to
             * this new frame, so whatever OP_MK_THUNK captures sits one
             * real Env level below `scope`'s corresponding env. `skip`
             * is a dummy, name-less scope level (matches nothing, so
             * every lookup falls through it to `scope`) wrapped around
             * `scope` for exactly this purpose: it makes the compile-time
             * depth count include that one extra real frame, so it lines
             * back up with where the name actually lives at runtime. */
            size_t n = node->as.let1.count;
            AstBinding *bindings = node->as.let1.bindings;
            /* M146: stack scratch for the common small-n case -- see
             * ASNIX_COMPILE_SCRATCH_MAX's own comment. */
            const char *names_stack[ASNIX_COMPILE_SCRATCH_MAX];
            size_t name_lens_stack[ASNIX_COMPILE_SCRATCH_MAX];
            int names_on_stack = (n <= ASNIX_COMPILE_SCRATCH_MAX);
            const char **names = names_on_stack ? names_stack : malloc(n * sizeof(char *));
            size_t *name_lens = names_on_stack ? name_lens_stack : malloc(n * sizeof(size_t));
            for (size_t i = 0; i < n; i++) {
                names[i] = bindings[i].name;
                name_lens[i] = bindings[i].name_len;
            }
            ScopeFrame frame = { .up = scope, .names = names, .name_lens = name_lens, .count = n };
            ScopeFrame skip = { .up = scope, .names = NULL, .name_lens = NULL, .count = 0 };

            ib_frame_alloc(b, (int64_t)n);
            for (size_t i = 0; i < n; i++) {
                ScopeFrame *value_scope = bindings[i].is_inherit ? &skip : &frame;
                if (try_cell_ref(b, bindings[i].value, value_scope)) {
                    /* nothing more to do -- OP_VAR_REF_CELL already emitted */
                } else if (is_thunk_free_literal(bindings[i].value)) {
                    compile_expr(b, bindings[i].value, value_scope);
                } else {
                    IrBuilder *thunk_b = ib_new();
                    compile_expr(thunk_b, bindings[i].value, value_scope);
                    ib_ret(thunk_b);
                    ib_mk_thunk(b, ib_finish(thunk_b));
                }
                ib_frame_set(b, (int64_t)i);
            }
            compile_expr(b, node->as.let1.body, &frame);
            ib_frame_exit(b);
            if (!names_on_stack) { free(names); free(name_lens); }
            return;
        }
        case AST_IF: {
            Ast *cond = node->as.if1.cond;
            Ast *then_node = node->as.if1.then_;
            Ast *else_node = node->as.if1.else_;

            /* M162: when the condition is LITERALLY a bare `?` test or a
             * bare comparison, compile directly against the op's own
             * internal true/false result (OP_HAS_ATTR_IF/OP_EQ_IF/
             * OP_NEQ_IF/OP_LT_IF/OP_GE_IF) instead of the generic
             * {producer op; OP_IF} shape -- see ir.h's comment on those
             * ops for the full rationale (the generic shape pays to tag/
             * push a TRUE/FALSE Value just for OP_IF to immediately pop
             * and re-validate a type the producer already knows for
             * certain). A constant-foldable comparison is deliberately
             * excluded (falls through to the generic path below,
             * unchanged) so this never interacts with try_fold_const_
             * binop's own separate optimization. */
            if (cond->kind == AST_HAS_ATTR && !cond->as.has_attr.attr_expr) {
                compile_expr(b, cond->as.has_attr.target, scope);
                uint32_t sym = asnix_intern(cond->as.has_attr.attr, cond->as.has_attr.attr_len);
                size_t if_site = ib_has_attr_if(b, sym);
                compile_expr(b, then_node, scope);
                size_t jmp_site = ib_jmp(b);
                size_t else_idx = ib_len(b);
                ib_patch_delta(b, if_site, else_idx);
                compile_expr(b, else_node, scope);
                size_t end_idx = ib_len(b);
                ib_patch_delta(b, jmp_site, end_idx);
                return;
            }
            if (cond->kind == AST_BINOP) {
                char cop = cond->as.binop.op;
                int is_cmp = (cop == 'e' || cop == 'n' || cop == 'l' || cop == 'G' || cop == 'g' || cop == 'L');
                int fold_is_bool, fold_is_float;
                int64_t fold_val;
                double fold_fval;
                if (is_cmp && !try_fold_const_binop(cond, &fold_is_bool, &fold_is_float, &fold_val, &fold_fval)) {
                    size_t if_site;
                    switch (cop) {
                        case 'e':
                        case 'n':
                        case 'l':
                        case 'G':
                            /* Already left-to-right, no operand swap -- same order plain
                             * ib_eq/ib_neq/ib_lt/ib_ge use for these four op chars. */
                            compile_expr(b, cond->as.binop.lhs, scope);
                            compile_expr(b, cond->as.binop.rhs, scope);
                            if (cop == 'e') if_site = ib_eq_if(b);
                            else if (cop == 'n') if_site = ib_neq_if(b);
                            else if (cop == 'l') if_site = ib_lt_if(b);
                            else if_site = ib_ge_if(b);
                            compile_expr(b, then_node, scope);
                            {
                                size_t jmp_site = ib_jmp(b);
                                size_t else_idx = ib_len(b);
                                ib_patch_delta(b, if_site, else_idx);
                                compile_expr(b, else_node, scope);
                                size_t end_idx = ib_len(b);
                                ib_patch_delta(b, jmp_site, end_idx);
                            }
                            return;
                        case 'g':
                            /* `a > b` == `b < a` -- same swapped-operand-push trick
                             * compiler.c's AST_BINOP case uses for plain `>` (M51's own
                             * comment there), reusing OP_LT_IF with ordinary then/else
                             * mapping (no branch swap needed here). */
                            compile_expr(b, cond->as.binop.rhs, scope);
                            compile_expr(b, cond->as.binop.lhs, scope);
                            if_site = ib_lt_if(b);
                            compile_expr(b, then_node, scope);
                            {
                                size_t jmp_site = ib_jmp(b);
                                size_t else_idx = ib_len(b);
                                ib_patch_delta(b, if_site, else_idx);
                                compile_expr(b, else_node, scope);
                                size_t end_idx = ib_len(b);
                                ib_patch_delta(b, jmp_site, end_idx);
                            }
                            return;
                        case 'L':
                            /* `a <= b` == `!(b < a)` -- same swap as `>` above, PLUS the
                             * compiled then/else branches themselves are swapped (the
                             * fallthrough-on-true side of OP_LT_IF(b,a) is "b < a is
                             * true", i.e. "a <= b is false", i.e. our ELSE; the jump-
                             * target side is our THEN). */
                            compile_expr(b, cond->as.binop.rhs, scope);
                            compile_expr(b, cond->as.binop.lhs, scope);
                            if_site = ib_lt_if(b);
                            compile_expr(b, else_node, scope);
                            {
                                size_t jmp_site = ib_jmp(b);
                                size_t then_idx = ib_len(b);
                                ib_patch_delta(b, if_site, then_idx);
                                compile_expr(b, then_node, scope);
                                size_t end_idx = ib_len(b);
                                ib_patch_delta(b, jmp_site, end_idx);
                            }
                            return;
                    }
                }
            }

            compile_expr(b, cond, scope);
            size_t if_site = ib_if(b);
            compile_expr(b, then_node, scope);
            size_t jmp_site = ib_jmp(b);
            size_t else_idx = ib_len(b);
            ib_patch_delta(b, if_site, else_idx);
            compile_expr(b, else_node, scope);
            size_t end_idx = ib_len(b);
            ib_patch_delta(b, jmp_site, end_idx);
            return;
        }
        case AST_LAMBDA: {
            if (!node->as.lambda.is_pattern) {
                IrBuilder *body_b = ib_new();
                const char *names[1] = { node->as.lambda.param };
                size_t name_lens[1] = { node->as.lambda.param_len };
                ScopeFrame frame = { .up = scope, .names = names, .name_lens = name_lens, .count = 1 };
                compile_expr(body_b, node->as.lambda.body, &frame);
                ib_ret(body_b);
                IrProgram *body_prog = ib_finish(body_b);
                /* Not a lambda-pattern -- `builtins.functionArgs` is `{}`
                 * here, matching real Nix (confirmed: `functionArgs (a:
                 * a)` == `{}`; a curried `a: b: a` is just two nested
                 * non-pattern lambdas, so this applies at every level --
                 * M37). */
                ib_lambda(b, 1, body_prog, (int64_t)asnix_bshared_make_attrs(NULL, NULL, 0).raw);
                return;
            }

            /* Lambda pattern (`{a, b ? d, ...}: body` / with an optional
             * `@name` alias either side): still arity-1 at the OP_CALL ABI
             * level -- op_call.S/Closure don't change at all. The call's
             * one real attrset argument lands in a throwaway "arg frame"
             * (depth 1 from the body's point of view), and a *second*
             * frame (depth 0) holds one thunk per declared field.
             *
             * That second frame is allocated via OP_FRAME_NEW *before* any
             * field's thunk is constructed (OP_FRAME_BIND_SLOT fills each
             * slot only after its thunk already exists) so every field's
             * thunk captures this frame, already-current, as its own
             * scope -- giving fields letrec-style mutual/forward
             * visibility, confirmed against real Nix
             * (`{a ? b+1, b ? 5}: a` applied to `{}` sees `b` from `a`'s
             * default even though `b` is declared after `a`). This is the
             * same "allocate the frame before constructing the thunks" fix
             * docs/architecture.md's M6 notes flag as what plain `let`
             * would need to become recursive -- applied narrowly to this
             * one synthetic frame, not to OP_LET/general `let` or attrset
             * bindings, which keep their documented non-recursive
             * limitation unchanged. */
            size_t nf = node->as.lambda.formals_count;
            IrBuilder *body_b = ib_new();

            const char *arg_names[1] = { node->as.lambda.has_as_name ? node->as.lambda.as_name : "" };
            size_t arg_name_lens[1] = { node->as.lambda.has_as_name ? node->as.lambda.as_name_len : 0 };
            ScopeFrame argframe = { .up = scope, .names = arg_names, .name_lens = arg_name_lens, .count = 1 };

            /* -- Eager validation (OP_CHECK_FORMALS), matching real Nix's
             * timing exactly: a type mismatch, a missing required field,
             * or (without `...`) an extra field all abort immediately on
             * application, even if the body never reads the offending
             * field (confirmed against nix-instantiate). -- */
            /* M146: stack scratch for the common small-nf case -- see
             * ASNIX_COMPILE_SCRATCH_MAX's own comment. All seven of this
             * case's scratch buffers (below, and field_names/
             * field_name_lens/fa_syms/fa_vals further down) switch
             * together on the SAME `nf_on_stack` flag, so a pattern with
             * more formals than the cap falls back to malloc for every
             * one of them uniformly, never a mix. */
            int nf_on_stack = (nf <= ASNIX_COMPILE_SCRATCH_MAX);
            uint32_t all_syms_stack[ASNIX_COMPILE_SCRATCH_MAX];
            uint32_t required_syms_stack[ASNIX_COMPILE_SCRATCH_MAX];
            uint32_t all_syms_sorted_stack[ASNIX_COMPILE_SCRATCH_MAX];
            const char *field_names_stack[ASNIX_COMPILE_SCRATCH_MAX];
            size_t field_name_lens_stack[ASNIX_COMPILE_SCRATCH_MAX];
            uint32_t fa_syms_stack[ASNIX_COMPILE_SCRATCH_MAX];
            Value fa_vals_stack[ASNIX_COMPILE_SCRATCH_MAX];

            uint32_t *all_syms = nf_on_stack ? all_syms_stack : malloc((nf ? nf : 1) * sizeof(uint32_t));
            size_t required_count = 0;
            for (size_t i = 0; i < nf; i++) {
                all_syms[i] = asnix_intern(node->as.lambda.formals[i].name, node->as.lambda.formals[i].name_len);
                if (!node->as.lambda.formals[i].default_) required_count++;
            }
            uint32_t *required_syms = nf_on_stack ? required_syms_stack : malloc((required_count ? required_count : 1) * sizeof(uint32_t));
            {
                size_t k = 0;
                for (size_t i = 0; i < nf; i++) {
                    if (!node->as.lambda.formals[i].default_) required_syms[k++] = all_syms[i];
                }
            }
            sort_syms(required_syms, required_count);
            uint32_t *all_syms_sorted = NULL;
            int64_t total_count = -1; /* `...`: no "unexpected argument" check at all */
            if (!node->as.lambda.has_ellipsis) {
                all_syms_sorted = nf_on_stack ? all_syms_sorted_stack : malloc((nf ? nf : 1) * sizeof(uint32_t));
                memcpy(all_syms_sorted, all_syms, nf * sizeof(uint32_t));
                sort_syms(all_syms_sorted, nf);
                total_count = (int64_t)nf;
            }

            ib_var_ref(body_b, 0, 0); /* the raw attrset arg: argframe is depth 0 until OP_FRAME_NEW below */
            ib_check_formals(body_b, (int64_t)required_count, required_syms, total_count, all_syms_sorted);
            if (!nf_on_stack) {
                free(all_syms);
                free(required_syms);
                free(all_syms_sorted);
            }

            /* -- The letrec-capable field frame. -- */
            ib_frame_alloc(body_b, (int64_t)nf);

            /* ll-only, no asnix equivalent, see ir.h's OP_MK_FORMALSMAP
             * comment: one shared descriptor for every REQUIRED (no
             * default) formal, instead of one Thunk each. Must run here,
             * right after ib_frame_alloc and before any per-formal
             * ib_frame_set below -- same "frame exists before anything
             * writes into it" ordering ib_check_formals/ib_frame_alloc
             * above already established for the thunk-based path. */
            {
                int formalsmap_on_stack = (nf <= ASNIX_COMPILE_SCRATCH_MAX);
                int64_t fm_syms_stack[ASNIX_COMPILE_SCRATCH_MAX];
                int64_t *fm_syms = formalsmap_on_stack ? fm_syms_stack : malloc((nf ? nf : 1) * sizeof(int64_t));
                for (size_t i = 0; i < nf; i++) {
                    fm_syms[i] = node->as.lambda.formals[i].default_
                                     ? -1
                                     : (int64_t)asnix_intern(node->as.lambda.formals[i].name,
                                                              node->as.lambda.formals[i].name_len);
                }
                ib_mk_formalsmap(body_b, (int64_t)nf, fm_syms);
                if (!formalsmap_on_stack) free(fm_syms);
            }

            const char **field_names = nf_on_stack ? field_names_stack : malloc((nf ? nf : 1) * sizeof(char *));
            size_t *field_name_lens = nf_on_stack ? field_name_lens_stack : malloc((nf ? nf : 1) * sizeof(size_t));
            for (size_t i = 0; i < nf; i++) {
                field_names[i] = node->as.lambda.formals[i].name;
                field_name_lens[i] = node->as.lambda.formals[i].name_len;
            }
            ScopeFrame fieldframe = { .up = &argframe, .names = field_names, .name_lens = field_name_lens, .count = nf };

            for (size_t i = 0; i < nf; i++) {
                /* A required (no default) formal already got its slot
                 * written directly by ib_mk_formalsmap above -- nothing
                 * left to compile or frame_set here, same way a Cell-
                 * aliased bare variable elsewhere skips the thunk path
                 * entirely. Only an optional-with-default formal reaches
                 * the rest of this loop body. */
                if (node->as.lambda.formals[i].default_ == NULL) continue;

                /* Each field's thunk body resolves names against
                 * `fieldframe` (siblings, incl. itself and later fields --
                 * letrec) with `argframe` one level up (the raw attrset,
                 * and -- if given -- its `@` alias). Built from synthetic
                 * AST nodes so it goes through the exact same compile_expr
                 * paths as hand-written source would: `if arg ? field then
                 * arg.field else default`. */
                Ast *arg_var = asnix_ast_new(AST_VAR);
                arg_var->as.var.name = arg_names[0];
                arg_var->as.var.name_len = arg_name_lens[0];

                Ast *select_node = asnix_ast_new(AST_SELECT);
                select_node->as.select.target = arg_var;
                select_node->as.select.attr = node->as.lambda.formals[i].name;
                select_node->as.select.attr_len = node->as.lambda.formals[i].name_len;

                Ast *arg_var2 = asnix_ast_new(AST_VAR);
                arg_var2->as.var.name = arg_names[0];
                arg_var2->as.var.name_len = arg_name_lens[0];
                Ast *has_node = asnix_ast_new(AST_HAS_ATTR);
                has_node->as.has_attr.target = arg_var2;
                has_node->as.has_attr.attr = node->as.lambda.formals[i].name;
                has_node->as.has_attr.attr_len = node->as.lambda.formals[i].name_len;

                Ast *if_node = asnix_ast_new(AST_IF);
                if_node->as.if1.cond = has_node;
                if_node->as.if1.then_ = select_node;
                if_node->as.if1.else_ = node->as.lambda.formals[i].default_;
                Ast *field_body = if_node;

                if (try_cell_ref(body_b, field_body, &fieldframe)) {
                    /* nothing more to do -- OP_VAR_REF_CELL already emitted */
                } else if (is_thunk_free_literal(field_body)) {
                    compile_expr(body_b, field_body, &fieldframe);
                } else {
                    IrBuilder *thunk_b = ib_new();
                    compile_expr(thunk_b, field_body, &fieldframe);
                    ib_ret(thunk_b);
                    ib_mk_thunk(body_b, ib_finish(thunk_b));
                }
                ib_frame_set(body_b, (int64_t)i);
            }

            compile_expr(body_b, node->as.lambda.body, &fieldframe);
            ib_ret(body_b);
            IrProgram *body_prog = ib_finish(body_b);
            if (!nf_on_stack) {
                free(field_names);
                free(field_name_lens);
            }

            /* `builtins.functionArgs` (M37): `{name: hasDefault}` for
             * every declared field, built once here (straight from
             * `node->as.lambda.formals`, still live) and stashed on the
             * closure -- confirmed against real Nix: `functionArgs ({a,
             * b ? 1, ...}: a)` == `{ a = false; b = true; }` (the `...`
             * ellipsis itself doesn't add an entry or change any field's
             * reported default-ness). */
            uint32_t *fa_syms = nf_on_stack ? fa_syms_stack : malloc((nf ? nf : 1) * sizeof(uint32_t));
            Value *fa_vals = nf_on_stack ? fa_vals_stack : malloc((nf ? nf : 1) * sizeof(Value));
            for (size_t i = 0; i < nf; i++) {
                fa_syms[i] = asnix_intern(node->as.lambda.formals[i].name, node->as.lambda.formals[i].name_len);
                fa_vals[i] = node->as.lambda.formals[i].default_ ? ASNIX_TRUE : ASNIX_FALSE;
            }
            Value function_args = asnix_bshared_make_attrs(fa_syms, fa_vals, (int64_t)nf);
            if (!nf_on_stack) {
                free(fa_syms);
                free(fa_vals);
            }

            ib_lambda(b, 1, body_prog, (int64_t)function_args.raw);
            return;
        }
        case AST_HAS_ATTR: {
            compile_expr(b, node->as.has_attr.target, scope);
            if (node->as.has_attr.attr_expr) {
                /* Dynamic `? ${expr}` -- same split as AST_SELECT's
                 * attr_expr case: push the name's value, let
                 * OP_HAS_ATTR_DYNAMIC intern/look it up at runtime (see
                 * asnix_bshared_has_attr_dynamic, builtins_shared.c). */
                compile_expr(b, node->as.has_attr.attr_expr, scope);
                ib_has_attr_dynamic(b);
                return;
            }
            uint32_t sym = asnix_intern(node->as.has_attr.attr, node->as.has_attr.attr_len);
            ib_has_attr(b, sym);
            return;
        }
        case AST_APP: {
            if (try_fused_native_call2(b, node, scope)) return;
            /* Real Nix call arguments are call-by-need, not call-by-
             * value: `(x: 1) (abort "never runs")` returns `1` without
             * ever evaluating the unused argument, and a lazy fixed
             * point like `let f = x: {a=1;}; self = f self; in self.a`
             * (idiomatic throughout nixpkgs -- lib.makeExtensible,
             * overlays, callPackage, `finalAttrs` patterns) depends on
             * `self` being passed to `f` as an unforced reference, never
             * evaluated at the call site at all (forcing an in-progress
             * thunk from the *inside* of its own computation is a real
             * cycle; merely passing it through unused is not -- real Nix
             * returns `1` here too, confirmed against `nix-instantiate`).
             * Previously this compiled the argument directly into the
             * caller's own code (eager, call-by-value), which aborted on
             * the first example and blackholed on the second -- a real,
             * previously-undiscovered core-evaluator bug, not a syntax
             * gap, found evaluating real nixpkgs (M25). Fixed the same
             * way every other lazy binding already works (AST_LET/
             * AST_ATTRSET above): wrap the argument expression in its
             * own OP_MK_THUNK, capturing the *caller's* env, instead of
             * compiling it inline -- OP_VAR_REF's existing force-on-read
             * logic (already correct, untouched) then forces it, if and
             * only if the callee's body ever actually reads the
             * parameter. */

            /* perf (round 7, Env-frame fusion, ll-only): a syntactic
             * application chain (`f a b c` is AST_APP(AST_APP(AST_APP(
             * f,a),b),c)) walks leftward to its base here. A chain of
             * depth >= 2 compiles to ONE OP_CALL_CHAIN instead of N
             * separate OP_CALL(1)s -- see ir.h's OP_CALL_CHAIN comment
             * and op_call.c's ll_op_call_chain for the runtime half,
             * which is purely an allocation-batching optimization: it
             * either produces the IDENTICAL result N ordinary calls
             * would have, or falls back to exactly that N-calls
             * sequence, never anything else. Depth 1 (the common,
             * already-fast base case) falls through unchanged below. */
            size_t chain_len = 1;
            Ast *chain_walk = node;
            while (chain_walk->as.app.fn->kind == AST_APP) {
                chain_len++;
                chain_walk = chain_walk->as.app.fn;
            }
            if (chain_len >= 2) {
                int args_on_stack = (chain_len <= ASNIX_COMPILE_SCRATCH_MAX);
                Ast *args_stack[ASNIX_COMPILE_SCRATCH_MAX];
                Ast **args = args_on_stack ? args_stack : malloc(chain_len * sizeof(Ast *));
                Ast *cur = node;
                for (size_t i = chain_len; i-- > 0;) {
                    args[i] = cur->as.app.arg;
                    cur = cur->as.app.fn;
                }
                compile_expr(b, chain_walk->as.app.fn, scope);
                for (size_t i = 0; i < chain_len; i++) {
                    compile_app_arg(b, args[i], scope);
                }
                ib_call_chain(b, (int64_t)chain_len);
                if (!args_on_stack) free(args);
                return;
            }

            compile_expr(b, node->as.app.fn, scope);
            compile_app_arg(b, node->as.app.arg, scope);
            ib_call(b, 1);
            return;
        }
        case AST_BINOP: {
            /* M136: fold first, when safe -- see try_fold_const_binop's
             * own header comment above for exactly which cases qualify
             * and why every excluded case (overflow/div-by-zero/non-int
             * operators) must fall through to the ordinary path below
             * unchanged rather than risk silently diverging from the
             * runtime op's own checked behavior. */
            int fold_is_bool, fold_is_float;
            int64_t fold_val;
            double fold_fval;
            if (try_fold_const_binop(node, &fold_is_bool, &fold_is_float, &fold_val, &fold_fval)) {
                if (fold_is_bool) {
                    if (fold_val) ib_push_true(b);
                    else ib_push_false(b);
                } else if (fold_is_float) {
                    /* Same compile-time-heap-allocate-once rationale as
                     * AST_FLOAT's own case above -- a folded float result
                     * is still a genuine 2-word heap box, not an
                     * immediate, so it's materialized once here and
                     * pooled via ib_push_const, never re-allocated per
                     * eval. */
                    Value fv = asnix_make_float_value(fold_fval);
                    ib_push_const(b, (int64_t)fv.raw);
                } else {
                    ib_push_int(b, fold_val);
                }
                return;
            }
            /* M51: `>` and `<=` evaluate their RIGHT operand first, not
             * left -- found while chasing numtide/nix-conformance's
             * eval-okay-compare-operand-order.nix (blocked until this
             * milestone by the same float-literal parse failure as
             * every other file in this pass, but the bug it actually
             * exercises is this evaluation-order rule, unrelated to
             * floats per se). Real Nix's own grammar explains why
             * (`~/nix/src/libexpr/parser.y`): `a > b` desugars to
             * `__lessThan b a` and `a <= b` to `!(__lessThan b a)` --
             * i.e. the operands are swapped at the AST level before
             * `lessThan`'s own (curried, forces-its-first-applied-arg-
             * first) application forces them, so `b` (the ORIGINAL
             * right operand) is always forced before `a`. `<` and `>=`
             * need no such swap: they desugar to `__lessThan a b` /
             * `!(__lessThan a b)`, i.e. already plain left-to-right.
             * Verified directly against `nix-instantiate --eval` with
             * abort/throw canaries on both sides of both `>` and `<=`
             * (see this milestone's session transcript). Implemented by
             * compiling the operands in SWAPPED order and reusing the
             * existing `<` IR op (ib_lt) with its own operands therefore
             * *also* swapped relative to the source -- `a > b` becomes
             * "push b, push a, lt" (computing "b < a", i.e. exactly "a >
             * b"); `a <= b` becomes "push b, push a, lt, not" (computing
             * "!(b < a)", i.e. exactly "a <= b") -- rather than adding a
             * new IR op: OP_GT/OP_LE (ib_gt/ib_le, op_cmp.S's
             * asnix_op_gt/asnix_op_le) are left completely unused by the
             * compiler after this change (not deleted -- harmless dead
             * code, same convention as any other op this project has
             * ever superseded without ripping out). */
            if (node->as.binop.op == 'g') {
                compile_expr(b, node->as.binop.rhs, scope);
                compile_expr(b, node->as.binop.lhs, scope);
                ib_lt(b);
                return;
            }
            if (node->as.binop.op == 'L') {
                compile_expr(b, node->as.binop.rhs, scope);
                compile_expr(b, node->as.binop.lhs, scope);
                ib_lt(b);
                ib_not(b);
                return;
            }
            compile_expr(b, node->as.binop.lhs, scope);
            compile_expr(b, node->as.binop.rhs, scope);
            switch (node->as.binop.op) {
                case '+': ib_add(b); return;
                case '-': ib_sub(b); return;
                case '*': ib_mul(b); return;
                case 'e': ib_eq(b); return;
                case 'n': ib_neq(b); return;
                case 'l': ib_lt(b); return;
                case 'G': ib_ge(b); return;
                case 'u': ib_update(b); return;
                case 'c': ib_concat(b); return;
                case '/': ib_div(b); return;
            }
            return;
        }
        case AST_AND: {
            /* Short-circuit, compiled branch-shaped like `if lhs then rhs
             * else false` (NOT as an eager binop) -- see ir.h's OP_EQ
             * comment: the untaken side's IR is simply never reached by
             * the jmp-chain dispatch loop, so it's provably never
             * evaluated (verified against real nix-instantiate with
             * `false && (assert false; true)`, see tests/e2e). */
            compile_expr(b, node->as.logic.lhs, scope);
            size_t if_site = ib_if(b);
            compile_expr(b, node->as.logic.rhs, scope);
            size_t jmp_site = ib_jmp(b);
            size_t else_idx = ib_len(b);
            ib_patch_delta(b, if_site, else_idx);
            ib_push_false(b);
            size_t end_idx = ib_len(b);
            ib_patch_delta(b, jmp_site, end_idx);
            return;
        }
        case AST_OR: {
            /* `if lhs then true else rhs` -- same short-circuit rationale as AST_AND. */
            compile_expr(b, node->as.logic.lhs, scope);
            size_t if_site = ib_if(b);
            ib_push_true(b);
            size_t jmp_site = ib_jmp(b);
            size_t else_idx = ib_len(b);
            ib_patch_delta(b, if_site, else_idx);
            compile_expr(b, node->as.logic.rhs, scope);
            size_t end_idx = ib_len(b);
            ib_patch_delta(b, jmp_site, end_idx);
            return;
        }
        case AST_NOT:
            compile_expr(b, node->as.not1.operand, scope);
            ib_not(b);
            return;
        case AST_ASSERT: {
            /* `if cond then body else <fatal, naming the failed cond>` --
             * same branch shape as AST_IF, reusing OP_IF/OP_JMP wholesale;
             * only the "else" side differs (OP_ASSERT_FAIL instead of
             * compiled code), see op_assert.S. */
            compile_expr(b, node->as.assert1.cond, scope);
            size_t if_site = ib_if(b);
            compile_expr(b, node->as.assert1.body, scope);
            size_t jmp_site = ib_jmp(b);
            size_t else_idx = ib_len(b);
            ib_patch_delta(b, if_site, else_idx);

            static const char prefix[] = "asnix: assertion '";
            static const char suffix[] = "' failed, aborting\n";
            size_t text_len = node->as.assert1.cond_text_len;
            size_t total = sizeof(prefix) - 1 + text_len + sizeof(suffix) - 1;
            char *msg = asnix_arena_alloc(total);
            memcpy(msg, prefix, sizeof(prefix) - 1);
            memcpy(msg + sizeof(prefix) - 1, node->as.assert1.cond_text, text_len);
            memcpy(msg + sizeof(prefix) - 1 + text_len, suffix, sizeof(suffix) - 1);
            ib_assert_fail(b, msg, total);

            size_t end_idx = ib_len(b);
            ib_patch_delta(b, jmp_site, end_idx);
            return;
        }
        case AST_STRING: {
            size_t count = node->as.string.count;
            AstStringPart *parts = node->as.string.parts;

            int all_literal = 1;
            for (size_t i = 0; i < count; i++) {
                if (parts[i].is_expr) { all_literal = 0; break; }
            }

            if (all_literal) {
                /* No interpolation at all (the common case, including every
                 * plain "..."/''...'' literal) -- fold into one compile-time
                 * constant, exactly like pre-interpolation M5 did. */
                size_t total = 0;
                for (size_t i = 0; i < count; i++) total += parts[i].text_len;
                void *mem = asnix_arena_alloc(8 + total);
                *(int64_t *)((char *)mem + ASNIX_STRING_OFF_LEN) = (int64_t)total;
                char *out = (char *)mem + ASNIX_STRING_OFF_BYTES;
                for (size_t i = 0; i < count; i++) {
                    memcpy(out, parts[i].text, parts[i].text_len);
                    out += parts[i].text_len;
                }
                Value v = { .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_PTR };
                ib_push_const(b, (int64_t)v.raw);
                return;
            }

            /* At least one interpolated part: push each literal chunk (as
             * its own string constant) and each interpolated expression's
             * raw result, then let OP_STR_CONCAT (runtime) coerce the
             * non-string ones and concatenate everything in source order --
             * see docs/architecture.md's strings scope notes.
             *
             * A zero-length literal chunk is skipped entirely (neither
             * pushed nor counted) -- the lexer unconditionally emits a
             * TOK_STR_PART on both sides of a `${...}` interpolation, even
             * when there's no literal text there at all (lexer.c's own
             * comment: "possibly zero-length, e.g. between two adjacent
             * ${...}${...} interpolations"), so the overwhelmingly common
             * `"${x}"` shape (no literal padding) otherwise compiles to a
             * 3-part concat -- two pointless empty-string constants plus
             * `x` -- instead of just `x` itself. Measured directly on a
             * real nixpkgs closure (`./asnix d.nix`, ffmpeg's closure):
             * 46.2% of all interpolated string literals (1,945 of 4,208)
             * have at least one empty literal part, 2,335 total. Each
             * empty part otherwise costs one compile-time-constant String
             * heap object (one per distinct source occurrence, reused via
             * the pool from then on) plus one extra VALSTACK push and one
             * extra coerce-and-copy iteration in `concat_strings_impl`
             * (value_util.c) on EVERY evaluation. `concat_strings_impl`
             * already handles any count >= 1 correctly with no special
             * casing (confirmed by reading it directly) -- a bare `"${x}"`
             * now compiles straight to `ib_str_concat(b, 1)`, which still
             * does the real coercion/path-existence-check/context-union
             * work a single interpolated value needs, just without the
             * two no-op concatenation partners. */
            size_t emit_count = 0;
            for (size_t i = 0; i < count; i++) {
                if (!parts[i].is_expr && parts[i].text_len == 0) continue;
                if (parts[i].is_expr) {
                    compile_expr(b, parts[i].expr, scope);
                } else {
                    Value v = asnix_make_string_value(parts[i].text, parts[i].text_len);
                    ib_push_const(b, (int64_t)v.raw);
                }
                emit_count++;
            }
            ib_str_concat(b, (int64_t)emit_count);
            return;
        }
        case AST_LIST: {
            /* M53: each element is wrapped in its own thunk (captured over
             * the enclosing scope), mirroring the *exact* pattern the
             * non-rec AST_ATTRSET branch above already uses for binding
             * values, and AST_APP uses for call arguments (M25) -- real
             * Nix list elements are genuinely lazy per-element
             * (`builtins.elemAt [(abort "x") 1] 1` must NOT abort; a list
             * literal's own construction must not force any element,
             * only actually selecting/using one does). Previously this
             * called compile_expr directly inline, so the literal's own
             * construction bytecode unconditionally ran every element's
             * code (including calling `throw`/`abort` if an element was
             * such a call) the moment the list value itself was forced to
             * WHNF (e.g. by any consumer needing its ASNIX_TAG_LIST tag/
             * count/elems pointer) -- a real, previously-documented-but-
             * deliberately-unfixed gap (see M34's scope notes) that
             * surfaced as a conformance failure once a list literal
             * holding a canary `throw` was passed through `builtins.
             * foldl'` (eval-okay-foldlStrict-lazy-elements.nix): forcing
             * the list argument's own thunk (needed just to read its
             * tag) ran the old inline bytecode for *every* element,
             * firing the canary regardless of which elements the fold
             * ever touched. OP_MK_LIST (op_mk_list.S) itself is unchanged
             * -- it already just moves `count` already-evaluated Values
             * off VALSTACK into a list object, oblivious to whether each
             * one is a forced Value or (now) a Thunk; element consumers
             * were already audited in M34 to force-before-read where
             * needed (map/mapAttrs already produced genuine Thunk
             * elements before this change), so this doesn't introduce a
             * new element shape, just a new *producer* of the same
             * already-handled shape. */
            for (size_t i = 0; i < node->as.list.count; i++) {
                if (try_cell_ref(b, node->as.list.elems[i], scope)) {
                    /* nothing more to do -- OP_VAR_REF_CELL already emitted */
                } else if (is_thunk_free_literal(node->as.list.elems[i])) {
                    compile_expr(b, node->as.list.elems[i], scope);
                } else {
                    IrBuilder *elem_b = ib_new();
                    compile_expr(elem_b, node->as.list.elems[i], scope);
                    ib_ret(elem_b);
                    ib_mk_thunk(b, ib_finish(elem_b));
                }
            }
            ib_mk_list(b, (int64_t)node->as.list.count);
            return;
        }
        case AST_ATTRSET: {
            size_t n = node->as.attrset.count;
            /* M146: stack scratch for the common small-n case -- see
             * ASNIX_COMPILE_SCRATCH_MAX's own comment. `small_n` doubles
             * as the stack-vs-heap decision for every scratch buffer in
             * this whole case (hnames/hname_lens and visible_sorted_syms
             * below included: their own sizes -- hidden_count, visible_
             * count -- are always <= n, so whenever n itself fits the
             * cap, so do they) -- deliberately the SAME threshold the
             * insertion-sort-vs-qsort choice right below already uses,
             * so a literal that takes the "large" path for one takes it
             * for the other too, never a mix. */
            uint32_t syms_stack[ASNIX_COMPILE_SCRATCH_MAX];
            size_t order_stack[ASNIX_COMPILE_SCRATCH_MAX];
            uint32_t sorted_syms_stack[ASNIX_COMPILE_SCRATCH_MAX];
            int small_n = (n <= ASNIX_ATTRSET_SORT_INSERTION_MAX);
            uint32_t *syms = small_n ? syms_stack : malloc(n * sizeof(uint32_t));
            size_t *order = small_n ? order_stack : malloc(n * sizeof(size_t));
            for (size_t i = 0; i < n; i++) {
                syms[i] = asnix_intern(node->as.attrset.bindings[i].name, node->as.attrset.bindings[i].name_len);
                order[i] = i;
            }
            /* perf (M142): was an unconditional O(n^2) insertion sort on the
             * claim "n is a source-level binding count, always tiny" --
             * false for real nixpkgs (pkgs/top-level/all-packages.nix is a
             * single ~4,280-binding rec {} literal). A naive global switch
             * to qsort for every n REGRESSED (+0.65% instructions on the
             * real d.nix eval, measured directly): qsort's per-comparison
             * indirect-call overhead costs more than insertion sort's
             * O(n^2) saves across the overwhelming majority of small
             * literals, and even a second attempt that built a {sym,idx}
             * pairs array unconditionally (to make insertion sort's own
             * comparisons branch-direct instead of double-indirected
             * through order[]/syms[]) still regressed (+0.24%), from the
             * extra malloc/free pair that version paid on every single
             * call regardless of n. Hybrid, below: insertion sort directly
             * on order[]/syms[] (byte-for-byte the original, zero-extra-
             * allocation algorithm) below the threshold; a {sym,idx} pairs
             * array + qsort, built and freed ONLY on this branch, for the
             * rare large literal. Names are already unique here (duplicate-
             * key literals are merged earlier via reopen_attrset_into), so
             * no two syms ever compare equal and stability is a non-issue
             * either way. */
            if (small_n) {
                for (size_t i = 1; i < n; i++) {
                    size_t j = i;
                    while (j > 0 && syms[order[j - 1]] > syms[order[j]]) {
                        size_t tmp = order[j - 1];
                        order[j - 1] = order[j];
                        order[j] = tmp;
                        j--;
                    }
                }
            } else {
                sort_attrset_bindings_large(syms, order, n);
            }
            uint32_t *sorted_syms = small_n ? sorted_syms_stack : malloc(n * sizeof(uint32_t));
            for (size_t i = 0; i < n; i++) sorted_syms[i] = syms[order[i]];

            if (!node->as.attrset.is_rec) {
                /* Plain `{ ... }`: lazy (each binding's value is wrapped
                 * in a thunk captured over the *enclosing* scope), but
                 * deliberately NOT recursive -- a binding can't reference
                 * `self` or a sibling key, matching real Nix (only
                 * `rec { ... }` gets that). `inherit` here is just an
                 * ordinary binding whose value happens to be an
                 * AST_VAR/AST_SELECT that also resolves against `scope`,
                 * so it needs no special handling.
                 *
                 * M80: EXCEPT for a multi-name `inherit (expr) a b;`
                 * clause's own synthetic hidden binding (parser.c,
                 * AstBinding.is_hidden) -- that one DOES need a slot
                 * every sibling select can resolve its `AST_VAR` against
                 * (so `expr` is evaluated once, shared, instead of once
                 * per inherited name), without granting ordinary
                 * siblings any new visibility into EACH OTHER (this
                 * attrset is still non-`rec`). A tiny extra frame holding
                 * only the hidden slot(s), with `.up = scope`, gives
                 * exactly that: every ordinary binding's lookup still
                 * falls through it to `scope` unchanged (so when there
                 * are no hidden bindings at all -- the overwhelming
                 * majority of plain `{ ... }` literals -- this is
                 * mechanically identical to the pre-M80 code, just with
                 * one always-empty, zero-cost frame wrapped around it). */
                size_t hidden_count = 0;
                for (size_t i = 0; i < n; i++) if (node->as.attrset.bindings[i].is_hidden) hidden_count++;
                if (hidden_count == 0) {
                    /* M114: the overwhelming common case -- no `inherit
                     * (expr) a b;` clause needs a shared hidden slot, so
                     * NOTHING here needs its own frame at all. Real Nix's
                     * own non-recursive ExprAttrs::eval (eval.cc, ~line
                     * 1315) never allocates an Env for a plain `{ ... }`
                     * literal: every binding's value is bound directly
                     * against the ENCLOSING scope, because non-`rec`
                     * siblings never see each other -- there is no shared
                     * scope for a frame to provide. Before this case
                     * existed, `ib_frame_alloc(b, 0)` ran unconditionally
                     * here, costing one real (if zero-slot) Env allocation
                     * per plain attrset literal evaluated, for nothing
                     * (confirmed: 189,914 of these on real nixpkgs's
                     * ffmpeg.outPath, 20% of asnix's total env-count gap
                     * against real Nix for the same evaluation). This
                     * mirrors the `hidden_count > 0` branch below exactly,
                     * minus the hidden frame itself -- every binding/
                     * dyn-binding value compiles straight against `scope`,
                     * and `order[i]` (not `i`) is used so values are
                     * pushed in the SAME sorted-symbol order `sorted_syms`
                     * is already in, matching what `ib_mk_attrs` needs
                     * (identical to the `visible_sorted_syms` loop below,
                     * just with `visible_count == n` so no filtering or
                     * copy is needed). */
                    for (size_t i = 0; i < n; i++) {
                        AstBinding *bind = &node->as.attrset.bindings[order[i]];
                        if (try_cell_ref(b, bind->value, scope)) {
                            /* nothing more to do -- OP_VAR_REF_CELL already emitted */
                        } else if (is_thunk_free_literal(bind->value)) {
                            compile_expr(b, bind->value, scope);
                        } else {
                            IrBuilder *thunk_b = ib_new();
                            compile_expr(thunk_b, bind->value, scope);
                            ib_ret(thunk_b);
                            ib_mk_thunk(b, ib_finish(thunk_b));
                        }
                    }
                    ib_mk_attrs(b, (int64_t)n, sorted_syms);
                    /* M67: `${expr} = value;` entries -- see the matching
                     * comment below (the `hidden_count > 0` branch); same
                     * reasoning applies verbatim with `scope` standing in
                     * for `&hidden_frame` (which, with zero hidden slots,
                     * was always mechanically identical to `scope` at
                     * runtime anyway -- this path just stops paying for
                     * the frame that fell through to it). */
                    for (size_t i = 0; i < node->as.attrset.dyn_count; i++) {
                        compile_expr(b, node->as.attrset.dyn_bindings[i].name_expr, scope);
                        if (try_cell_ref(b, node->as.attrset.dyn_bindings[i].value, scope)) {
                            /* nothing more to do -- OP_VAR_REF_CELL already emitted */
                        } else if (is_thunk_free_literal(node->as.attrset.dyn_bindings[i].value)) {
                            compile_expr(b, node->as.attrset.dyn_bindings[i].value, scope);
                        } else {
                            IrBuilder *thunk_b = ib_new();
                            compile_expr(thunk_b, node->as.attrset.dyn_bindings[i].value, scope);
                            ib_ret(thunk_b);
                            ib_mk_thunk(b, ib_finish(thunk_b));
                        }
                        ib_insert_dynamic_attr(b);
                    }
                    if (!small_n) { free(syms); free(order); free(sorted_syms); }
                    return;
                }
                /* hidden_count <= n, so whenever `small_n` is true (n fit
                 * the stack cap) so does hidden_count -- same flag reused,
                 * see ASNIX_COMPILE_SCRATCH_MAX's comment. */
                const char *hnames_stack[ASNIX_COMPILE_SCRATCH_MAX];
                size_t hname_lens_stack[ASNIX_COMPILE_SCRATCH_MAX];
                const char **hnames = small_n ? hnames_stack : malloc((hidden_count ? hidden_count : 1) * sizeof(char *));
                size_t *hname_lens = small_n ? hname_lens_stack : malloc((hidden_count ? hidden_count : 1) * sizeof(size_t));
                size_t hi = 0;
                for (size_t i = 0; i < n; i++) {
                    if (node->as.attrset.bindings[i].is_hidden) {
                        hnames[hi] = node->as.attrset.bindings[i].name;
                        hname_lens[hi] = node->as.attrset.bindings[i].name_len;
                        hi++;
                    }
                }
                ScopeFrame hidden_frame = { .up = scope, .names = hnames, .name_lens = hname_lens, .count = hidden_count };
                /* `skip`: SAME fix AST_LET's own bare-`inherit` case needs
                 * (see that case's comment) and for the identical reason
                 * -- by the time any binding's thunk is built here,
                 * `ib_frame_alloc` just above already switched the
                 * *runtime* ENV to `hidden_frame`, one real level below
                 * `scope`'s corresponding env. The hidden source's own
                 * value (`src_expr`) must still resolve against `scope`
                 * (it can't see itself, and for a single-hidden-slot
                 * clause there's nothing else in `hidden_frame` for it to
                 * see either), but compiling it against `scope` DIRECTLY
                 * -- as an earlier version of this fix did -- underc-
                 * ounts the depth by exactly one real frame: a name
                 * found at `scope`'s own depth-0 would wrongly compile to
                 * `ib_var_ref(0, slot)`, which at runtime reads THIS
                 * frame's slot 0 (the hidden binding's own, possibly-
                 * still-being-forced thunk) instead of walking one more
                 * hop up to `scope`'s real env -- a genuine self-
                 * reference, not merely a wrong answer (confirmed via
                 * numtide/nix-conformance's eval-okay-getattrpos.nix:
                 * `{ inherit (pos) column line; }` blackholed forcing
                 * `pos` against itself before this fix). `skip`'s own
                 * `count == 0` means it can never match a lookup itself,
                 * so every name still resolves in `scope` exactly as
                 * before -- it exists purely to add that one compensating
                 * hop. */
                ScopeFrame skip = { .up = scope, .names = NULL, .name_lens = NULL, .count = 0 };
                ib_frame_alloc(b, (int64_t)hidden_count);
                hi = 0;
                for (size_t i = 0; i < n; i++) {
                    if (!node->as.attrset.bindings[i].is_hidden) continue;
                    if (try_cell_ref(b, node->as.attrset.bindings[i].value, &skip)) {
                        /* nothing more to do -- OP_VAR_REF_CELL already emitted */
                    } else if (is_thunk_free_literal(node->as.attrset.bindings[i].value)) {
                        compile_expr(b, node->as.attrset.bindings[i].value, &skip);
                    } else {
                        IrBuilder *thunk_b = ib_new();
                        compile_expr(thunk_b, node->as.attrset.bindings[i].value, &skip);
                        ib_ret(thunk_b);
                        ib_mk_thunk(b, ib_finish(thunk_b));
                    }
                    ib_frame_set(b, (int64_t)hi);
                    hi++;
                }
                size_t visible_count = n - hidden_count;
                uint32_t visible_sorted_syms_stack[ASNIX_COMPILE_SCRATCH_MAX];
                uint32_t *visible_sorted_syms = small_n ? visible_sorted_syms_stack : malloc((visible_count ? visible_count : 1) * sizeof(uint32_t));
                size_t vi = 0;
                for (size_t i = 0; i < n; i++) {
                    if (node->as.attrset.bindings[order[i]].is_hidden) continue;
                    if (try_cell_ref(b, node->as.attrset.bindings[order[i]].value, &hidden_frame)) {
                        /* nothing more to do -- OP_VAR_REF_CELL already emitted */
                    } else if (is_thunk_free_literal(node->as.attrset.bindings[order[i]].value)) {
                        compile_expr(b, node->as.attrset.bindings[order[i]].value, &hidden_frame);
                    } else {
                        IrBuilder *thunk_b = ib_new();
                        compile_expr(thunk_b, node->as.attrset.bindings[order[i]].value, &hidden_frame);
                        ib_ret(thunk_b);
                        ib_mk_thunk(b, ib_finish(thunk_b));
                    }
                    visible_sorted_syms[vi++] = sorted_syms[i];
                }
                ib_mk_attrs(b, (int64_t)visible_count, visible_sorted_syms);
                /* M67: `${expr} = value;` entries (M28) -- compiled
                 * against the enclosing `scope` (here, `&hidden_frame`,
                 * which falls through to `scope` for everything except
                 * the synthetic hidden names above -- identical to plain
                 * `scope` whenever there are none), same as every static
                 * binding's value just above: a plain (non-`rec`)
                 * attrset is never self-referential at all (confirmed
                 * unchanged by this pass: `{ ${k} = a; a = 1; }` -- no
                 * `rec` -- is "undefined variable 'a'" in real Nix too,
                 * same as any other plain-`{}` sibling reference). See
                 * the `rec` branch below for the case that DID need to
                 * change (a dynamic binding's own value seeing a STATIC
                 * `rec` sibling). */
                for (size_t i = 0; i < node->as.attrset.dyn_count; i++) {
                    compile_expr(b, node->as.attrset.dyn_bindings[i].name_expr, &hidden_frame);
                    if (try_cell_ref(b, node->as.attrset.dyn_bindings[i].value, &hidden_frame)) {
                        /* nothing more to do -- OP_VAR_REF_CELL already emitted */
                    } else if (is_thunk_free_literal(node->as.attrset.dyn_bindings[i].value)) {
                        compile_expr(b, node->as.attrset.dyn_bindings[i].value, &hidden_frame);
                    } else {
                        IrBuilder *thunk_b = ib_new();
                        compile_expr(thunk_b, node->as.attrset.dyn_bindings[i].value, &hidden_frame);
                        ib_ret(thunk_b);
                        ib_mk_thunk(b, ib_finish(thunk_b));
                    }
                    ib_insert_dynamic_attr(b);
                }
                ib_frame_exit(b);
                if (!small_n) { free(hnames); free(hname_lens); free(visible_sorted_syms); }
            } else {
                /* `rec { ... }`: same frame-first trick as AST_LET, keyed
                 * by original (unsorted) binding index so the ScopeFrame
                 * slot a binding's thunk-body references lines up with
                 * the OP_FRAME_SET that filed it. Once every binding is
                 * filed, OP_FRAME_GET(slot) reads each one straight back
                 * off the frame in sorted order -- reusing the exact same
                 * OP_MK_ATTRS the non-rec path uses. Deliberately
                 * OP_FRAME_GET, NOT OP_VAR_REF (M29 fix): OP_VAR_REF
                 * forces on read, which would eagerly force *every*
                 * `rec {}` field the instant the literal is constructed
                 * -- a real bug found evaluating real nixpkgs (confirmed:
                 * `(rec { a = abort "x"; b = 2; }).b` aborted instead of
                 * returning `2`, same root-cause class as M25's call-by-
                 * value argument bug). Each binding is already a fresh,
                 * never-shared Thunk (OP_MK_THUNK) at this point, so a
                 * raw, non-forcing read-back is exactly right -- the
                 * Thunk itself still gets forced later, lazily, the
                 * first time something actually selects that field. */
                AstBinding *bindings = node->as.attrset.bindings;
                const char *names_stack[ASNIX_COMPILE_SCRATCH_MAX];
                size_t name_lens_stack[ASNIX_COMPILE_SCRATCH_MAX];
                const char **names = small_n ? names_stack : malloc(n * sizeof(char *));
                size_t *name_lens = small_n ? name_lens_stack : malloc(n * sizeof(size_t));
                for (size_t i = 0; i < n; i++) {
                    names[i] = bindings[i].name;
                    name_lens[i] = bindings[i].name_len;
                }
                ScopeFrame frame = { .up = scope, .names = names, .name_lens = name_lens, .count = n };
                /* `skip`: see the matching comment in AST_LET's case --
                 * same reason a bare `inherit x;` here can't just compile
                 * against `scope` directly (OP_FRAME_ALLOC already
                 * switched the runtime ENV by the time any binding's
                 * thunk is built). */
                ScopeFrame skip = { .up = scope, .names = NULL, .name_lens = NULL, .count = 0 };

                /* M57c: `__overrides` (legacy feature -- allows a `rec {}`
                 * field's value to be swapped out, with the swap visible
                 * to every *other* field that references it, something
                 * `//` alone can't do since by the time you `//` the
                 * result, sibling thunks have already captured the
                 * original value through this same frame). Detected
                 * purely by binding NAME at compile time (a plain, non-
                 * `rec` `{ __overrides = ...; }` is NOT special -- verified
                 * against real Nix: only `rec {}`'s own ExprAttrs::eval
                 * special-cases it, eval.cc). */
                int64_t overrides_slot = -1;
                for (size_t i = 0; i < n; i++) {
                    if (bindings[i].name_len == 11 && memcmp(bindings[i].name, "__overrides", 11) == 0) {
                        overrides_slot = (int64_t)i;
                        break;
                    }
                }
                /* M-skip-cell: mark this frame before it's used below, so
                 * try_cell_ref refuses to alias any sibling binding's
                 * slot when this set has __overrides -- see ScopeFrame's
                 * own has_overrides comment. */
                frame.has_overrides = (overrides_slot >= 0);

                ib_frame_alloc(b, (int64_t)n);
                for (size_t i = 0; i < n; i++) {
                    ScopeFrame *value_scope = bindings[i].is_inherit ? &skip : &frame;
                    if (try_cell_ref(b, bindings[i].value, value_scope)) {
                        /* nothing more to do -- OP_VAR_REF_CELL already emitted */
                    } else if (is_thunk_free_literal(bindings[i].value)) {
                        compile_expr(b, bindings[i].value, value_scope);
                    } else {
                        IrBuilder *thunk_b = ib_new();
                        compile_expr(thunk_b, bindings[i].value, value_scope);
                        ib_ret(thunk_b);
                        ib_mk_thunk(b, ib_finish(thunk_b));
                    }
                    ib_frame_set(b, (int64_t)i);
                }
                if (overrides_slot >= 0) {
                    /* `syms` (built above, BEFORE the symbol-id sort into
                     * `order`/`sorted_syms`) is already in original,
                     * per-slot order -- exactly what OP_REC_OVERRIDES_MERGE
                     * needs (see ir.h). Replaces the plain path's
                     * OP_FRAME_GET*n + OP_MK_ATTRS pair with the one op
                     * that merges `__overrides` in first.
                     *
                     * M80: NOT filtered for `is_hidden` -- combining
                     * `__overrides` with a multi-name `inherit (expr) a
                     * b;` clause in the SAME `rec {}` is an extreme edge
                     * case no fixture in this project's corpora exercises
                     * (confirmed: neither eval-okay-inherit-from.nix nor
                     * any other numtide/nix-conformance case does), so a
                     * hidden binding here would still leak into the
                     * final attrset as a real (if unguessable-named,
                     * `\x01`-prefixed) attribute -- a known, narrow,
                     * deliberately out-of-scope gap rather than a silent
                     * wrong answer on anything currently tested. */
                    ib_rec_overrides_merge(b, (int64_t)n, syms, overrides_slot);
                } else {
                    /* M80: `is_hidden` bindings (parser.c's synthetic
                     * `inherit (expr) a b;` shared-source slot) get a
                     * frame slot like any other binding above (so the
                     * sibling selects referencing it by name resolve
                     * correctly, each one sharing -- not re-running --
                     * the one evaluation of `expr`) but must never
                     * become a real attribute of the resulting attrset:
                     * skip them here. Whenever none exist (the
                     * overwhelming majority of `rec {}` literals), this
                     * loop is identical to the pre-M80 unfiltered one. */
                    size_t visible_count = 0;
                    for (size_t i = 0; i < n; i++) if (!bindings[order[i]].is_hidden) visible_count++;
                    uint32_t visible_sorted_syms_stack[ASNIX_COMPILE_SCRATCH_MAX];
                    uint32_t *visible_sorted_syms = small_n ? visible_sorted_syms_stack : malloc((visible_count ? visible_count : 1) * sizeof(uint32_t));
                    size_t vi = 0;
                    for (size_t i = 0; i < n; i++) {
                        if (bindings[order[i]].is_hidden) continue;
                        ib_frame_get(b, (int64_t)order[i]);
                        visible_sorted_syms[vi++] = sorted_syms[i];
                    }
                    ib_mk_attrs(b, (int64_t)visible_count, visible_sorted_syms);
                    if (!small_n) free(visible_sorted_syms);
                }
                /* M67: `${expr} = value;` entries (M28), merged in HERE --
                 * still inside the rec's own frame, before `ib_frame_exit`
                 * -- using `&frame` (not `scope`) so a dynamic binding's
                 * OWN name/value expression can see its STATIC siblings
                 * via the ordinary OP_FRAME_GET path, same as any other
                 * `rec {}` binding. Verified directly against real
                 * `nix-instantiate --eval`: `let bString = "b"; in (rec {
                 * "${bString}" = a; a = true; }).b` is `true` (the
                 * *dynamic* binding's value `a` sees the static sibling
                 * `a = true;`) -- previously compiled against the
                 * *enclosing* `scope` instead, giving "undefined variable
                 * 'a'" (found via numtide/nix-conformance's eval-okay-
                 * dynamic-attrs.nix/-bare.nix). This is NOT the same
                 * question ast.h's own AST_ATTRSET.dyn_bindings comment
                 * answers (and that comment is still correct, unchanged by
                 * this fix): a *static* sibling still cannot reference a
                 * *dynamically-named* attribute (`rec { ${k}=1; b=a; }`
                 * with `k="a"` is still "undefined variable 'a'", verified
                 * -- static bindings are filed into fixed, compile-time-
                 * known frame slots, and a dynamic binding never gets one,
                 * so there's nothing for `b=a` to resolve to regardless of
                 * this fix). Only the *dynamic* binding's own name/value
                 * expressions gain the ability to read (never write) the
                 * frame -- applied to the attrs value still sitting on the
                 * VALSTACK from `ib_mk_attrs`/`ib_rec_overrides_merge`
                 * just above, exactly as the non-`rec` path (below) does
                 * with `scope` right after ITS OWN `ib_mk_attrs`. */
                for (size_t i = 0; i < node->as.attrset.dyn_count; i++) {
                    compile_expr(b, node->as.attrset.dyn_bindings[i].name_expr, &frame);
                    if (try_cell_ref(b, node->as.attrset.dyn_bindings[i].value, &frame)) {
                        /* nothing more to do -- OP_VAR_REF_CELL already emitted */
                    } else if (is_thunk_free_literal(node->as.attrset.dyn_bindings[i].value)) {
                        compile_expr(b, node->as.attrset.dyn_bindings[i].value, &frame);
                    } else {
                        IrBuilder *thunk_b = ib_new();
                        compile_expr(thunk_b, node->as.attrset.dyn_bindings[i].value, &frame);
                        ib_ret(thunk_b);
                        ib_mk_thunk(b, ib_finish(thunk_b));
                    }
                    ib_insert_dynamic_attr(b);
                }
                ib_frame_exit(b);
                if (!small_n) { free(names); free(name_lens); }
            }
            if (!small_n) { free(syms); free(order); free(sorted_syms); }
            return;
        }
        case AST_SELECT: {
            if (!node->as.select.default_) {
                compile_expr(b, node->as.select.target, scope);
                if (node->as.select.attr_expr) {
                    /* Dynamic `.${expr}` -- the attribute name isn't known
                     * until runtime, so there's no sym to intern here; the
                     * name expr's *value* (a string) is pushed and
                     * OP_SELECT_DYNAMIC (src/asm/ops/op_select_dynamic.S)
                     * interns/looks it up at runtime (see
                     * asnix_bshared_select_dynamic, builtins_shared.c). */
                    compile_expr(b, node->as.select.attr_expr, scope);
                    ib_select_dynamic(b);
                    return;
                }
                uint32_t sym = asnix_intern(node->as.select.attr, node->as.select.attr_len);
                ib_select(b, sym);
                return;
            }
            /* `expr.attr or default` -- real Nix's grammar (`parser.y`:
             * `expr_simple '.' attrpath OR_KW expr_select`) attaches `or`
             * to the WHOLE preceding dotted chain, and `ExprSelect::eval`
             * (eval.cc) walks EVERY segment in one loop, falling through
             * to `default` the instant any one of them misses or the
             * current value isn't an attrset -- not just the last
             * segment. parse_select (parser.c) builds a chain of nested
             * single-segment AST_SELECT nodes (one per `.segment`,
             * `target` pointing at the previous node) and, on seeing a
             * trailing `or`, stamps the SAME `default_` Ast* onto every
             * node of that one unbroken chain. So the first time we see a
             * non-NULL default_ here, `node` may be the OUTERMOST of a
             * multi-segment chain, not a lone single-segment select --
             * walk back down `target` while it's itself an AST_SELECT
             * with the exact same `default_` pointer (true for every
             * node of *this* chain and only this chain, by construction)
             * to collect every segment, outermost first. Interior/
             * innermost chain members are never visited by compile_expr
             * directly (nothing recurses into them except this walk,
             * which bypasses the normal `compile_expr(target)` call
             * below) -- that's what keeps `default` from being compiled
             * (and potentially evaluated) more than once per chain. A
             * lone `a.x or d` is just the one-segment case of this same
             * walk (loop body runs once). */
            Ast *default_ast = node->as.select.default_;
            size_t seg_cap = 4, seg_len = 0;
            Ast **segments = malloc(seg_cap * sizeof(Ast *));
            Ast *chain_node = node;
            for (;;) {
                if (seg_len == seg_cap) {
                    seg_cap *= 2;
                    segments = realloc(segments, seg_cap * sizeof(Ast *));
                }
                segments[seg_len++] = chain_node;
                Ast *next = chain_node->as.select.target;
                if (next->kind != AST_SELECT || next->as.select.default_ != default_ast) {
                    /* `next` is the chain's true base expression (not
                     * part of this shared-default chain) -- compile it
                     * once, before any segment's select-or op. */
                    compile_expr(b, next, scope);
                    break;
                }
                chain_node = next;
            }
            /* `segments` was collected outermost-first (descending via
             * `target`), but real Nix evaluates a dotted chain left to
             * right -- base, then innermost segment, ..., then outermost
             * -- so emit in reverse. */
            size_t *try_sites = malloc(seg_len * sizeof(size_t));
            for (size_t i = 0; i < seg_len; i++) {
                Ast *seg = segments[seg_len - 1 - i];
                if (seg->as.select.attr_expr) {
                    compile_expr(b, seg->as.select.attr_expr, scope);
                    try_sites[i] = ib_select_dynamic_or(b);
                } else {
                    uint32_t sym = asnix_intern(seg->as.select.attr, seg->as.select.attr_len);
                    try_sites[i] = ib_select_or(b, sym);
                }
            }
            free(segments);
            /* Every segment's miss-jump (OP_SELECT_OR/OP_SELECT_DYNAMIC_OR's
             * delta) shares this ONE landing pad: if every segment hit,
             * execution falls through all of them in sequence and must
             * jump PAST the default's code to the shared end; if any one
             * segment misses, it jumps straight here instead -- same
             * two-site patch-after-the-fact shape AST_IF uses for
             * then/else, just with N try-sites instead of one. */
            size_t jmp_site = ib_jmp(b);
            size_t default_idx = ib_len(b);
            for (size_t i = 0; i < seg_len; i++) {
                ib_patch_delta(b, try_sites[i], default_idx);
            }
            free(try_sites);
            compile_expr(b, default_ast, scope);
            size_t end_idx = ib_len(b);
            ib_patch_delta(b, jmp_site, end_idx);
            return;
        }
        case AST_WITH: {
            /* Lazy, same as `let`/attrset bindings (M6): the with-expr is
             * compiled as its own thunk body captured over the *outer*
             * scope (it can't see names `with` itself introduces), so a
             * `with` whose value is never actually consulted (every name
             * in `body` resolves lexically) never forces it -- matches
             * real Nix (verified: `with (builtins.trace "x" {}); 5` never
             * prints the trace). */
            if (try_cell_ref(b, node->as.with1.attrs, scope)) {
                /* nothing more to do -- OP_VAR_REF_CELL already emitted */
            } else if (is_thunk_free_literal(node->as.with1.attrs)) {
                compile_expr(b, node->as.with1.attrs, scope);
            } else {
                IrBuilder *thunk_b = ib_new();
                compile_expr(thunk_b, node->as.with1.attrs, scope);
                ib_ret(thunk_b);
                ib_mk_thunk(b, ib_finish(thunk_b));
            }
            ib_with(b);
            ScopeFrame frame = { .up = scope, .names = NULL, .name_lens = NULL, .count = 0, .is_with = 1 };
            compile_expr(b, node->as.with1.body, &frame);
            /* M52: undo the OP_WITH above, mirroring AST_LET's
             * ib_frame_exit(). Without this, a non-tail `with` (e.g. one
             * sibling among several `with`s in the same list/attrset/call
             * position) leaves its 1-slot with-frame installed as ENV for
             * whatever gets compiled next, so a sibling's own OP_MK_THUNK
             * captures that leftover frame instead of the true enclosing
             * scope -- confirmed bug: `let foo = {bar=1;}; baz = {qux=2;};
             * in [ (with foo; bar) (with baz; qux) ]` aborted with "`with'
             * value is not an attrset" because the second `with`'s attrs
             * thunk, built while ENV was still the first with's leftover
             * frame, read the wrong slot once forced. */
            ib_with_exit(b);
            return;
        }
        case AST_PATH: {
            if (node->as.path.is_search) {
                /* `<name>` (M31): unlike an ordinary path literal below,
                 * this must resolve LAZILY, at evaluation time, not here
                 * at compile time -- see asnix_resolve_search_path_value's
                 * own comment above for why (real Nix's own `findFile`
                 * throws a catchable-by-tryEval exception on failure,
                 * which only makes sense if the lookup happens during
                 * evaluation). Pushes the search name as an ordinary
                 * compile-time string constant (same mechanism AST_STRING
                 * uses for an all-literal string), then
                 * OP_RESOLVE_SEARCH_PATH performs the actual lookup at
                 * runtime. */
                Value name_v = asnix_make_string_value(node->as.path.text, node->as.path.len);
                ib_push_const(b, (int64_t)name_v.raw);
                ib_resolve_search_path(b);
                return;
            }
            if (node->as.path.interp_count > 0) {
                /* M64: `./${foo}`/`~/${foo}`/`/${foo}`/... -- the first
                 * chunk resolves to a genuine Path constant right here
                 * (resolve_path_chunk, NOT resolve_path_literal: a
                 * trailing '/' is expected and must be preserved, not
                 * rejected -- see that function's comment), then every
                 * subsequent literal chunk becomes a plain String
                 * constant and every interpolated part compiles
                 * normally, and OP_PATH_CONCAT (runtime) concatenates
                 * them all and re-canonicalizes the result as a Path --
                 * exactly ExprConcatStrings's own treatment of a
                 * Path-first operand (see ir.h's OP_PATH_CONCAT
                 * comment), generalizing the plain AST_STRING case right
                 * above this one in this same switch. */
                Value first = resolve_path_chunk(node->as.path.text, node->as.path.len, node->as.path.is_home);
                ib_push_const(b, (int64_t)first.raw);
                for (size_t i = 0; i < node->as.path.interp_count; i++) {
                    AstStringPart *part = &node->as.path.interp_parts[i];
                    if (part->is_expr) {
                        compile_expr(b, part->expr, scope);
                    } else {
                        Value sv = asnix_make_string_value(part->text, part->text_len);
                        ib_push_const(b, (int64_t)sv.raw);
                    }
                }
                ib_path_concat(b, (int64_t)(1 + node->as.path.interp_count));
                return;
            }
            /* Resolved once, here, against g_compile_base_dir -- not at
             * eval time -- matching real Nix (a path literal's absolute
             * form is fixed by where it's *written*, verified against
             * nix-instantiate; see this file's top-of-file comment and
             * resolve_path_literal()). */
            char *abs = resolve_path_literal(node);
            size_t len = strlen(abs);
            Value v = asnix_make_path_value(abs, len);
            free(abs);
            ib_push_const(b, (int64_t)v.raw);
            return;
        }
        case AST_CURPOS: {
            /* M80 (see docs/architecture.md): `__curPos` -- a genuine
             * compile-time CONSTANT (the position is fixed by where the
             * token is WRITTEN, never by when/how often it's evaluated;
             * confirmed against real Nix: forcing the same `__curPos`
             * thunk twice, or via two different call paths, always gives
             * the identical `{file;line;column;}`), built once here the
             * same way AST_PATH's own literal case just above does.
             *
             * `file`: M82 made this exact, not approximate --
             * `g_compile_file_path` is THIS compile call's own resolved
             * file path (every real caller, eval_file.c, passes its own
             * `resolved` path through asnix_compile/asnix_compile_scoped
             * now), so a `__curPos` token inside an IMPORTED file
             * correctly reports that file, not the outermost file asnix
             * was invoked on. Only falls back to
             * `asnix_eval_top_level_file()` when `g_compile_file_path`
             * is NULL, which happens for exactly one caller
             * (`asnix_eval_expr_source`, CLI `--arg`/`--argstr`
             * expression evaluation, which has no file of its own) plus
             * this file's own unit tests. `line`/`column` are exact
             * regardless -- M80's whole point (see parser.c's
             * curpos_line_col) is that those two, unlike
             * `unsafeGetAttrPos`'s per-ATTRIBUTE position (still
             * approximate, out of scope -- see docs/architecture.md's
             * M82 notes), only ever need ONE token's position (the
             * `__curPos` occurrence itself). */
            const char *file = g_compile_file_path ? g_compile_file_path : asnix_eval_top_level_file();
            if (!file) file = "";
            uint32_t syms[3];
            Value vals[3];
            syms[0] = asnix_intern("file", 4);
            vals[0] = asnix_make_string_value(file, strlen(file));
            syms[1] = asnix_intern("line", 4);
            vals[1] = asnix_make_int(node->as.curpos.line);
            syms[2] = asnix_intern("column", 6);
            vals[2] = asnix_make_int(node->as.curpos.column);
            Value v = asnix_bshared_make_attrs(syms, vals, 3);
            ib_push_const(b, (int64_t)v.raw);
            return;
        }
    }
}

/* The root env (constructed by eval_file.c) has sixteen slots: the
 * `builtins` attrset, plus `import`/`map`/`abort`/`toString`/
 * `removeAttrs`/`isNull`/`throw`/`derivation`/`baseNameOf`/`dirOf`/
 * `fromTOML`/`placeholder`/`scopedImport`/`derivationStrict`/`fetchGit`
 * bare -- names real Nix exposes as bare globals in addition to
 * `builtins.X` (see builtins.h's asnix_make_import_primop comment). This
 * scope frame makes `builtins.toString 5` and bare `import ./x`/`map f
 * xs`/`abort "msg"`/`toString 5`/`removeAttrs set ["a"]`/`isNull x`/
 * `throw "msg"`/`derivation {...}`/`baseNameOf x`/`dirOf x`/`fromTOML s`/
 * `placeholder "out"`/`scopedImport scope path`/`derivationStrict {...}`/
 * `fetchGit {...}` (M58: `derivation` itself is now a Nix-level wrapper
 * around this, see derivation.h; M82: `fetchGit`, see
 * builtins_fetchgit.c) all resolve like any other variable reference.
 * Shared by asnix_compile and asnix_compile_scoped (M54) below -- both
 * need exactly this same outermost frame, just with a possible extra
 * frame spliced in front of it for the latter. */
static void init_builtins_frame(ScopeFrame *f) {
    /* M82: "fetchGit" appended as slot 15 -- one of real Nix's bare
     * root-scope globals too (every primop `EvalState::addPrimOp`
     * registers without a `__` prefix, same rule as every other name in
     * this list; see builtins.h's comment and builtins_fetchgit.c's own
     * header comment for why this specific one was needed). Must stay
     * in the same order as eval_file.c's make_root_env_ex. */
    static const char *builtins_names[16] = {
        "builtins", "import", "map", "abort", "toString", "removeAttrs", "isNull", "throw", "derivation",
        "baseNameOf", "dirOf", "fromTOML", "placeholder", "scopedImport", "derivationStrict", "fetchGit" };
    static size_t builtins_name_lens[16] = { 8, 6, 3, 5, 8, 11, 6, 5, 10, 10, 5, 8, 11, 12, 16, 8 };
    f->up = NULL;
    f->names = builtins_names;
    f->name_lens = builtins_name_lens;
    f->count = 16;
    f->is_with = 0;
    f->has_overrides = 0;
}

IrProgram *asnix_compile(Ast *root, const char *base_dir, const char *file_path) {
    g_compile_base_dir = base_dir;
    g_compile_file_path = file_path;
    IrBuilder *b = ib_new();
    ScopeFrame builtins_frame;
    init_builtins_frame(&builtins_frame);
    compile_expr(b, root, &builtins_frame);
    ib_halt(b);
    return ib_finish(b);
}

/*
 * M54: `builtins.scopedImport scope path` -- like `asnix_compile` above,
 * but with one extra NAMED (not `with`-style fallback) ScopeFrame spliced
 * in between the root `builtins_frame` and `root`'s own top-level
 * expression, holding `scope`'s attribute names. This mirrors cppnix's
 * own implementation exactly (`scopedImport`, ~/nix/src/libexpr/
 * primops.cc): it builds a brand-new innermost StaticEnv seeded from
 * `vScope->attrs()`'s names, whose `up` is the ordinary base/global
 * StaticEnv -- i.e. a plain lexical binding (closer to `let` than
 * `with`), which is why `scope`'s names take priority over (shadow) the
 * ordinary bare globals/builtins with the same name (confirmed directly:
 * "Variables from scope shadow built-ins with the same name" in
 * `scopedImport`'s own real-Nix doc comment, and exercised by
 * numtide/nix-conformance's eval-okay-import.nix, which overrides bare
 * `import`/`scopedImport`/`builtins` this way). A real `with` (is_with=1)
 * would NOT give this shadowing priority -- compile_expr's AST_VAR case
 * (above) always lets a lexical/named-frame match win over any enclosing
 * `with`'s fallback regardless of relative nesting, but a `with`'s own
 * fallback never beats an outer NAMED frame either, which is the wrong
 * priority order for this feature. `extra_names`/`extra_lens` must stay
 * alive for the whole of this one compile call (the caller, eval_file.c's
 * asnix_eval_file_scoped, keeps them on the C stack for exactly that
 * long); `extra_count` may be 0 (an empty `scope` attrset), in which case
 * this frame simply never matches anything and every name falls through
 * to `builtins_frame` as usual. */
IrProgram *asnix_compile_scoped(Ast *root, const char *base_dir, const char *file_path,
                                 const char **extra_names, size_t *extra_lens, size_t extra_count) {
    g_compile_base_dir = base_dir;
    g_compile_file_path = file_path;
    IrBuilder *b = ib_new();
    ScopeFrame builtins_frame;
    init_builtins_frame(&builtins_frame);
    ScopeFrame extra_frame = {
        .up = &builtins_frame, .names = extra_names, .name_lens = extra_lens, .count = extra_count, .is_with = 0 };
    compile_expr(b, root, &extra_frame);
    ib_halt(b);
    return ib_finish(b);
}
