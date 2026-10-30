#define _DEFAULT_SOURCE

#include "flake_eval.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ll/ir.h"
#include "base64.h"
#include "builtins_fetchgit.h"
#include "builtins_shared.h"
#include "derivation.h"
#include "eval_file.h"
#include "nar.h"
#include "string_context.h"
#include "symtab.h"
#include "value_util.h"

static _Noreturn void flake_fatal(const char *msg) {
    fprintf(stderr, "ll: --flake: %s\n", msg);
    exit(1);
}

static int exclude_git_filter(void *ctx, const char *path, const char *type) {
    (void)ctx;
    (void)type;
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    return strcmp(base, ".git") != 0;
}

/* vendored verbatim from nix's own src/libflake/call-flake.nix (ported
 * from the same vendoring in asnix's own src/c/flake_eval.c) -- see
 * src/c/call-flake.nix (kept as a real file too, for diffing against
 * upstream). Embedded as a string constant rather than read from disk
 * at runtime, same convention compiler.c's fetchurl.nix embedding
 * already uses. */
static const char *const CALL_FLAKE_NIX =
    "lockFileStr:\n"
    "overrides:\n"
    "fetchTreeFinal:\n"
    "let\n"
    "  inherit (builtins) mapAttrs;\n"
    "  lockFile = builtins.fromJSON lockFileStr;\n"
    "  resolveInput =\n"
    "    inputSpec: if builtins.isList inputSpec then getInputByPath lockFile.root inputSpec else inputSpec;\n"
    "  getInputByPath =\n"
    "    nodeName: path:\n"
    "    if path == [ ] then\n"
    "      nodeName\n"
    "    else\n"
    "      getInputByPath\n"
    "        (resolveInput lockFile.nodes.${nodeName}.inputs.${builtins.head path})\n"
    "        (builtins.tail path);\n"
    "  allNodes = mapAttrs (\n"
    "    key: node:\n"
    "    let\n"
    "      hasOverride = overrides ? ${key};\n"
    "      isRelative = node.locked.type or null == \"path\" && builtins.substring 0 1 node.locked.path != \"/\";\n"
    "      parentNode = allNodes.${getInputByPath lockFile.root node.parent};\n"
    "      sourceInfo =\n"
    "        if hasOverride then\n"
    "          overrides.${key}.sourceInfo\n"
    "        else if isRelative then\n"
    "          parentNode.sourceInfo\n"
    "        else\n"
    "          fetchTreeFinal (node.info or { } // removeAttrs node.locked [ \"dir\" ]);\n"
    "      subdir = overrides.${key}.dir or node.locked.dir or \"\";\n"
    "      outPath =\n"
    "        if !hasOverride && isRelative then\n"
    "          parentNode.outPath + (if node.locked.path == \"\" then \"\" else \"/\" + node.locked.path)\n"
    "        else\n"
    "          sourceInfo.outPath + (if subdir == \"\" then \"\" else \"/\" + subdir);\n"
    "      flake = import (outPath + \"/flake.nix\");\n"
    "      inputs = mapAttrs (inputName: inputSpec: allNodes.${resolveInput inputSpec}.result) (\n"
    "        node.inputs or { }\n"
    "      );\n"
    "      outputs = flake.outputs (inputs // { self = result; });\n"
    "      result =\n"
    "        outputs\n"
    "        // sourceInfo\n"
    "        // {\n"
    "          inherit outPath;\n"
    "          inherit inputs;\n"
    "          inherit outputs;\n"
    "          inherit sourceInfo;\n"
    "          _type = \"flake\";\n"
    "        };\n"
    "    in\n"
    "    {\n"
    "      result =\n"
    "        if node.flake or true then\n"
    "          assert builtins.isFunction flake.outputs;\n"
    "          result\n"
    "        else\n"
    "          sourceInfo // { inherit sourceInfo outPath; };\n"
    "      inherit outPath sourceInfo;\n"
    "    }\n"
    "  ) lockFile.nodes;\n"
    "in\n"
    "allNodes.${lockFile.root}.result\n";

/* Builds {outPath; narHash;} for a local, already-on-disk directory,
 * WITHOUT copying it into the Nix store -- correct for exactly the two
 * cases this is used for: the root flake (the directory the user
 * passed on the command line IS the "previously fetched tree" real
 * Nix's own call-flake.nix `overrides` mechanism exists for -- see
 * that file's own header comment) and an absolute `path:`-type locked
 * input (which fetchTreeFinal below also resolves this way, matching
 * how a plain path literal/`builtins.path` already works in this
 * codebase: narHash is real and computed for provenance, but outPath
 * is simply the real, existing path, not a synthesized store path). */
static Value make_local_source_info(const char *dir) {
    uint8_t digest[32];
    if (ll_hash_path_nar_sha256(dir, exclude_git_filter, NULL, digest) != 0) {
        flake_fatal("failed to load libcrypto -- enter the flake devShell");
    }
    size_t b64len = ll_base64_encoded_length(32);
    char narhash_b64[64];
    ll_base64_encode(digest, 32, narhash_b64);
    char narhash_str[80];
    snprintf(narhash_str, sizeof(narhash_str), "sha256-%.*s", (int)b64len, narhash_b64);

    Value out_path_str = ll_make_string_value(dir, strlen(dir));
    LlStringContext ctx;
    ll_ctx_init(&ctx);
    ll_ctx_add_opaque(&ctx, dir);
    ll_string_attach_context(out_path_str, &ctx);

    uint32_t syms[2];
    Value vals[2];
    syms[0] = ll_intern("outPath", 7);
    vals[0] = out_path_str;
    syms[1] = ll_intern("narHash", 7);
    vals[1] = ll_make_string_value(narhash_str, strlen(narhash_str));
    return ll_bshared_make_attrs(syms, vals, 2);
}

static char *fetch_tree_final_dup_string(Value v, const char *who) {
    Value forced = ll_force_for_print(v);
    if (ll_tag_of(forced) != LL_TAG_PTR) {
        char msg[200];
        snprintf(msg, sizeof(msg), "fetchTree: '%s' must be a string", who);
        flake_fatal(msg);
    }
    int64_t len;
    const char *bytes = ll_ptr_bytes(forced, &len);
    char *out = malloc((size_t)len + 1);
    memcpy(out, bytes, (size_t)len);
    out[len] = '\0';
    return out;
}

/* `prim_fetchFinalTree` -- dispatches a locked, non-override,
 * non-relative-path node's `{type; ...}` attrset (exactly a
 * `node.info or {} // removeAttrs node.locked ["dir"]` value, per
 * call-flake.nix's own call site) to a real fetch, matching real Nix's
 * own internal primop of the same name. Only "path" (absolute) and
 * "git" are implemented -- an honest, narrow slice (ported from
 * asnix's own identically-scoped version): every other `type`
 * (github, gitlab, tarball, mercurial, ...) fails loudly, not
 * silently. */
Value ll_prim_fetch_tree_final(const Value *args, int64_t argc) {
    (void)argc;
    Value attrs = ll_force_for_print(args[0]);
    if (ll_tag_of(attrs) != LL_TAG_ATTRS) {
        flake_fatal("fetchTree: expected an attribute set");
    }

    uint32_t type_sym = ll_intern("type", 4);
    int64_t type_idx = ll_bshared_attrs_find(attrs, type_sym);
    if (type_idx < 0) flake_fatal("fetchTree: locked input has no 'type'");
    char *type = fetch_tree_final_dup_string(ll_bshared_attrs_value(attrs, type_idx), "type");

    if (strcmp(type, "path") == 0) {
        uint32_t path_sym = ll_intern("path", 4);
        int64_t path_idx = ll_bshared_attrs_find(attrs, path_sym);
        if (path_idx < 0) flake_fatal("fetchTree: 'path' input has no 'path'");
        char *path = fetch_tree_final_dup_string(ll_bshared_attrs_value(attrs, path_idx), "path");
        if (path[0] != '/') flake_fatal("fetchTree: 'path' input's 'path' must be absolute");
        Value result = make_local_source_info(path);
        free(path);
        free(type);
        return result;
    }

    if (strcmp(type, "git") == 0) {
        /* ll_prim_fetchGit fatals on an explicit 'type' key (its own
         * argument-validation loop treats it as unexpected) -- strip it
         * before forwarding url/ref/rev/shallow/submodules through. */
        uint32_t keep_syms[5];
        Value keep_vals[5];
        int64_t n = ll_bshared_attrs_count(attrs);
        int64_t k = 0;
        for (int64_t i = 0; i < n && k < 5; i++) {
            uint32_t sym = ll_bshared_attrs_sym(attrs, i);
            size_t klen;
            const char *kname = ll_sym_name(sym, &klen);
            if (klen == 4 && memcmp(kname, "type", 4) == 0) continue;
            if (klen == 7 && memcmp(kname, "narHash", 7) == 0) continue;
            keep_syms[k] = sym;
            keep_vals[k] = ll_bshared_attrs_value(attrs, i);
            k++;
        }
        Value filtered = ll_bshared_make_attrs(keep_syms, keep_vals, k);
        free(type);
        return ll_prim_fetchGit(&filtered, 1);
    }

    char msg[200];
    snprintf(msg, sizeof(msg), "fetchTree: input type '%s' is not supported yet", type);
    flake_fatal(msg);
}

/* Nix-level `import` (called from WITHIN call-flake.nix now) requires
 * an absolute path -- the CLI's own --flake argument is taken as-is
 * from argv, so a relative DIR needs resolving against the process
 * cwd first, same as eval_file.c's own resolve_import_path (static
 * there, so this is a minimal standalone copy of the same two steps:
 * cwd-prefix, then ll_canon_path). */
static char *resolve_flake_dir(const char *dir) {
    char *abs;
    if (dir[0] == '/') {
        abs = malloc(strlen(dir) + 1);
        memcpy(abs, dir, strlen(dir) + 1);
    } else {
        char cwd[4096];
        if (!getcwd(cwd, sizeof cwd)) {
            flake_fatal("cannot determine the current directory");
        }
        size_t cwd_len = strlen(cwd), dir_len = strlen(dir);
        abs = malloc(cwd_len + 1 + dir_len + 1);
        memcpy(abs, cwd, cwd_len);
        abs[cwd_len] = '/';
        memcpy(abs + cwd_len + 1, dir, dir_len + 1);
    }
    char *canon = ll_canon_path(abs);
    free(abs);
    return canon;
}

Value ll_eval_flake_outputs(const char *dir_arg) {
    char *dir = resolve_flake_dir(dir_arg);
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        char msg[400];
        snprintf(msg, sizeof(msg), "path '%s' is not a flake (because it's not a directory)", dir);
        flake_fatal(msg);
    }

    char flake_nix[300];
    snprintf(flake_nix, sizeof(flake_nix), "%s/flake.nix", dir);
    if (stat(flake_nix, &st) != 0 || !S_ISREG(st.st_mode)) {
        char msg[400];
        snprintf(msg, sizeof(msg), "path '%s' is not a flake (because it doesn't contain a 'flake.nix' file)", dir);
        flake_fatal(msg);
    }

    /* The lock file's own raw bytes are forwarded as-is -- call-flake.nix
     * itself does `builtins.fromJSON lockFileStr`, so no C-side JSON
     * parsing is needed here at all. A flake with no flake.lock gets a
     * trivial single-root-node lock synthesized in its place. */
    char lock_path[300];
    snprintf(lock_path, sizeof(lock_path), "%s/flake.lock", dir);
    char *lock_json;
    FILE *lf = fopen(lock_path, "rb");
    if (lf) {
        struct stat lst;
        if (fstat(fileno(lf), &lst) != 0 || !S_ISREG(lst.st_mode)) {
            fclose(lf);
            flake_fatal("'flake.lock' exists but is not a regular file");
        }
        size_t size = (size_t)lst.st_size;
        lock_json = malloc(size + 1);
        size_t n = fread(lock_json, 1, size, lf);
        lock_json[n] = '\0';
        fclose(lf);
    } else {
        lock_json = strdup("{\"version\":7,\"root\":\"root\",\"nodes\":{\"root\":{}}}");
    }
    Value lock_str = ll_make_string_value(lock_json, strlen(lock_json));
    free(lock_json);

    /* overrides = { root = { sourceInfo = <dir, NAR-hashed, not
     * store-copied>; }; } -- the root is always the directory given on
     * the command line, never fetched, exactly matching real Nix's own
     * --override-input-shaped escape hatch for "a previously fetched
     * tree already on disk" (call-flake.nix's own header comment on
     * `overrides`). */
    Value root_source_info = make_local_source_info(dir);
    uint32_t sourceinfo_sym = ll_intern("sourceInfo", 10);
    Value root_override = ll_bshared_make_attrs(&sourceinfo_sym, &root_source_info, 1);
    uint32_t root_sym = ll_intern("root", 4);
    Value overrides = ll_bshared_make_attrs(&root_sym, &root_override, 1);

    Value fetch_tree_final = ll_bshared_make_primop((void *)ll_prim_fetch_tree_final, 1);

    Value driver = ll_force_for_print(ll_eval_expr_source(CALL_FLAKE_NIX));
    Value step1 = ll_force_for_print(ll_apply(driver, &lock_str, 1));
    Value step2 = ll_force_for_print(ll_apply(step1, &overrides, 1));
    Value result = ll_force_for_print(ll_apply(step2, &fetch_tree_final, 1));
    return result;
}
