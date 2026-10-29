/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_NAR_H
#define LL_NAR_H

#include <stddef.h>
#include <stdint.h>

/*
 * Copies a local filesystem path (file, directory, or symlink) into the
 * content-addressed store the same way real Nix's own `addToStore`
 * (method=NixArchive/"recursive", hashAlgo=sha256) does: serialize it as
 * a NAR ("nix-archive-1", ~/nix/src/libutil/archive.cc -- string fields
 * are an 8-byte little-endian length, the raw bytes, then zero-padded to
 * the next 8-byte boundary; directory entries sorted ascending by name),
 * SHA-256 that, then feed the digest through the exact same
 * `makeStorePath("source", digest, name)` formula `compute_fixed_output_
 * path`'s sha256+recursive branch uses (store_path.h's
 * `ll_make_store_path_from_digest`). The NAR byte format must stay
 * byte-for-byte compatible with real Nix's own serializer, since any
 * divergence changes the resulting content hash and store path.
 *
 * Does NOT perform real Nix's actual filesystem side effect (writing the
 * content into `/nix/store/...`) -- same stance this project already
 * takes for `derivation`/`toFile` (no real store, no actual build ever
 * happens here): only the resulting path *string* is computed. Memoized
 * by literal input path so hashing the same local path twice within one
 * process doesn't re-walk/re-hash it.
 *
 * Fatal (prints a message and exits), matching this project's standing
 * convention, if `path` doesn't exist or names an unsupported file type
 * (anything other than a regular file, directory, or symlink) -- real
 * Nix's own `dumpPath` does the same. Writes the NUL-terminated
 * resulting store path into `out` (capacity `out_cap`).
 */
void ll_copy_path_to_store(const char *path, char *out, size_t out_cap);

/* Read-only lookup into `ll_copy_path_to_store`'s own memoization cache,
 * so a caller can skip its own redundant existence-check `lstat` when
 * the path was already successfully copied earlier in this process
 * (existence can't change mid-run for a path this project only ever
 * reads, never writes). Returns the cached store path (NUL-terminated,
 * owned by this module, valid for the process lifetime) or NULL on a
 * cache miss. */
const char *ll_copy_path_to_store_cache_lookup(const char *path);

/* Per-entry predicate for `ll_copy_path_to_store_ext` below --
 * `builtins.path`'s `filter`/`builtins.filterSource`'s own callback: the
 * CALLER (builtins_path.c) supplies a closure over the real Nix filter
 * function, this module just calls it with the entry's full filesystem
 * path and real Nix's own type-name string (`"regular"`/`"directory"`/
 * `"symlink"`/`"unknown"`). Returning 0 excludes the entry (and, for a
 * directory, its entire subtree) from the NAR exactly like real Nix's
 * own `PathFilter`. */
typedef int (*LlNarFilter)(void *ctx, const char *path, const char *type);

/* `builtins.path`/`builtins.filterSource`'s general form --
 * `ll_copy_path_to_store` above is the single common case (no filter,
 * NAR/"recursive" method, name = basename, no pinned hash) this
 * generalizes: a custom `name` (NUL-terminated; NULL means "use `path`'s
 * own basename"), an optional `filter` callback (NULL for none),
 * `recursive` selects the NixArchive (NAR, like `ll_copy_path_to_store`)
 * vs. Flat (bare file content, `path` MUST be a regular file)
 * content-addressing method -- both ultimately go through
 * `ll_compute_fixed_output_path` (derivation.h), the exact same formula
 * a fixed-output derivation's own `outputHash` uses -- and an optional
 * `expected_sha256` (NULL for none): if given, the path's OWN computed
 * content hash must match it exactly (fatal otherwise), matching real
 * Nix's `builtins.path { ...; sha256 = ...; }` pinned-hash check.
 * Unlike `ll_copy_path_to_store`, never memoized (the result depends on
 * `name`/`recursive`/`filter` too, not just `path`). */
void ll_copy_path_to_store_ext(const char *path, const char *name, int recursive, LlNarFilter filter,
                                   void *filter_ctx, const uint8_t expected_sha256[32], char *out, size_t out_cap);

/* `builtins.fetchGit` (builtins_fetchgit.c) needs the raw NAR sha256
 * digest itself, not just the final store path `ll_copy_path_to_store*`
 * compute from it -- real Nix's returned attrset carries both `outPath`
 * AND `narHash` (SRI form) side by side. Returns whatever
 * `ll_hash_sha256` returns (0 on success, nonzero if libcrypto couldn't
 * be loaded) -- same convention as every other hashing entry point in
 * this codebase. */
int ll_hash_path_nar_sha256(const char *path, LlNarFilter filter, void *filter_ctx, uint8_t out[32]);

#endif /* LL_NAR_H */
