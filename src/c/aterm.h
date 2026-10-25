/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef ASNIX_ATERM_H
#define ASNIX_ATERM_H

#include <stddef.h>

/*
 * ATerm (.drv) text serialization. M8's original version was scoped to
 * zero input derivations/zero input sources -- derivation-to-derivation
 * dependencies need knowing which attribute values reference another
 * derivation's output, which this project didn't have at the time. A
 * later milestone generalized the original single-fixed-"out"-output
 * assumption to an arbitrary, caller-supplied list of named outputs
 * (see derivation.c), matching real Nix's own `Outputs` section
 * (~/nix/src/libstore/derivation/aterm.cc's `unparse`), which is a map
 * from output name to (path, hashAlgo, hash) and is NOT folded into
 * `env`. M73 added real `InputDrvs`/`InputSrcs` sections (see
 * derivation.c's narrow, call-scoped input-reference collector) and
 * real per-output `hashAlgo`/`hash` fields for fixed-output derivations
 * -- see each struct's own comment below.
 *
 * Format and escaping rules ported from ~/nix/src/libstore/derivation/
 * aterm.cc and verified byte-for-byte against two real references before
 * writing this: numtide/nix-conformance's
 * drv/upstream/ia-advanced-attributes-defaults.drv, and a real
 * `nix-instantiate -E 'derivation { name = "asnix-test"; ... }'` run
 * locally -- both reproduced exactly by a throwaway Python port of this
 * same algorithm before any C was written.
 */

typedef struct {
    const char *key;
    const char *value;
} AtermEnvVar;

/* One entry of the ATerm's `Outputs` section: `name` is the output name
 * (e.g. "out", "bin"), `path` is that output's store path (or "" for the
 * masked/self-referential-hash pass, see store_path.h). `hash_algo`/
 * `hash` (M73) are NULL/unused for the plain input-addressed case this
 * project almost always produces (written as "" each, matching real
 * Nix's own InputAddressed/Deferred encoding) -- set them (hash_algo a
 * plain algorithm name like "sha256", hash its hex digest, no "r:"
 * prefix needed here since that's folded into the derivation.c caller's
 * own hash computation, not this struct) only for a fixed-output
 * derivation's "out" entry, confirmed directly against a real `.drv`:
 * `("out",path,"sha256","0d5f6015...")`, not `("out",path,"","")`. */
typedef struct {
    const char *name;
    const char *path;
    const char *hash_algo; /* NUL-terminated at the algorithm name's own end, not just output_hash's */
    const char *hash;
} AtermOutput;

/* One entry of the ATerm's `InputDrvs` section (M73): `drv_path` is the
 * referenced derivation's own `.drv` path (in the masked pass, see
 * derivation.c's caller, this is instead the hex string of that
 * derivation's own recursively-computed masked hash -- substituted by
 * the CALLER before building this list, not by this file, which just
 * serializes whatever string it's given); `output_names` is the sorted
 * list of that derivation's output names actually referenced (almost
 * always just `["out"]`), matching real Nix's `[(path,[outs...]),...]`
 * shape exactly. */
typedef struct {
    const char *drv_path;
    const char **output_names;
    size_t noutput_names;
} AtermInputDrv;

/* Builds "Derive([(name,path,hashAlgo,hash)...],[(drvPath,[outs...])...],
 * [srcPath...],<system>,<builder>,[args...],[(k,v)...])" with Nix's exact
 * escaping (only \n \r \t \" \\ are ever escaped). `outputs` MUST already
 * be sorted ascending by name (same convention `env` already has);
 * `input_drvs` MUST already be sorted ascending by `drv_path`, each
 * entry's own `output_names` ALSO already sorted; `input_srcs` MUST
 * already be sorted ascending. `env` MUST already be sorted ascending by
 * key. This function does not re-sort any of them. Returns a malloc'd,
 * NUL-terminated string (caller frees) -- this is scratch/build-time
 * text, not part of the runtime Value graph, so it doesn't need the
 * arena. */
char *asnix_build_derivation_aterm(const AtermOutput *outputs, size_t noutputs,
                                    const AtermInputDrv *input_drvs, size_t ninput_drvs, const char **input_srcs,
                                    size_t ninput_srcs, const char *system, const char *builder, const char **args,
                                    size_t nargs, const AtermEnvVar *env, size_t nenv);

/* M151: a per-derivation escape cache, keyed by the SOURCE POINTER of each
 * string handed to `asnix_build_derivation_aterm_cached` (not its content --
 * pointer identity is exactly what the caller already guarantees for the
 * common case this exists for: `derivation.c` builds up to 3 ATerm texts
 * per derivation -- masked/real/cache-hash passes -- from the SAME
 * `system`/`builder`/`args`/`input_srcs`/`env` arrays each time, varying
 * only the `outputs`/`input_drvs` sections between passes. Caching by
 * pointer means a string that's merely `strcmp`-equal but a DIFFERENT
 * buffer (e.g. a freshly computed output path reused as an env value)
 * correctly misses and gets escaped fresh -- no risk of a stale/wrong
 * hit from content that only looks the same. */
typedef struct AtermEscapeCache AtermEscapeCache;

/* `hint` is the caller's best guess at how many distinct strings it will
 * feed through this cache across all passes (doesn't need to be exact --
 * just sizes the initial table to avoid a few early growths). */
AtermEscapeCache *asnix_aterm_escape_cache_new(size_t hint);
void asnix_aterm_escape_cache_free(AtermEscapeCache *cache);

/* Same as `asnix_build_derivation_aterm`, but every string argument (each
 * `AtermOutput`/`AtermInputDrv`/`AtermEnvVar` field, `system`, `builder`,
 * every `args`/`input_srcs` entry) is escaped through `cache`: a string
 * whose POINTER was already escaped earlier via this same cache is
 * reused verbatim (skipping the scan-and-copy work entirely), and a
 * first-seen pointer is escaped normally, then recorded for later reuse.
 * `cache` may be NULL, in which case this is identical to
 * `asnix_build_derivation_aterm` (no caching) -- `asnix_build_derivation_
 * aterm` itself is now just this function called with `cache=NULL`. */
char *asnix_build_derivation_aterm_cached(const AtermOutput *outputs, size_t noutputs,
                                           const AtermInputDrv *input_drvs, size_t ninput_drvs,
                                           const char **input_srcs, size_t ninput_srcs, const char *system,
                                           const char *builder, const char **args, size_t nargs,
                                           const AtermEnvVar *env, size_t nenv, AtermEscapeCache *cache);

#endif /* ASNIX_ATERM_H */
