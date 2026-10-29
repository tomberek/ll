/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_STORE_PATH_H
#define LL_STORE_PATH_H

#include <stddef.h>
#include <stdint.h>

/*
 * Nix store path computation, ported from ~/nix/src/libstore/store-dir-config.cc
 * (`StoreDirConfig::makeStorePath`) and ~/nix/src/libutil/hash.cc
 * (`compressHash`).
 *
 * Deliberately has NO crypto dependency: computing the actual SHA-256
 * digests is the *caller's* job (via src/c/hash_shim.h), so this module
 * is pure byte/string manipulation, independently testable against known
 * digest bytes without needing any hash implementation at all.
 *
 * Algorithm (the general case; `makeStorePath` in the real source):
 *   preimage = type + ":" + hashWithAlgoPrefix + ":" + storeDir + ":" + name
 *   digest   = sha256(preimage)             -- computed by the caller
 *   compressed = compressHash(digest, 20)    -- XOR-fold 32 bytes down to 20
 *   path     = storeDir + "/" + base32(compressed) + "-" + name
 */

#define LL_STORE_DIR "/nix/store"

/* Assembles "type:hashWithAlgoPrefix:storeDir:name" into `out` (size
 * `out_cap`, NUL-terminated). Returns the length written (not counting
 * the NUL), or 0 if it wouldn't fit. */
size_t ll_store_path_preimage(const char *type, const char *hash_with_algo, const char *name, char *out,
                                  size_t out_cap);

/* Given a 32-byte SHA-256 digest of a preimage string (see above),
 * writes the full store path "/nix/store/<32-char-base32>-<name>" into
 * `out` (NUL-terminated). Returns the length written (not counting the
 * NUL), or 0 if it wouldn't fit. */
size_t ll_make_store_path_from_digest(const uint8_t digest[32], const char *name, char *out, size_t out_cap);

/* Validates a store path `name` component against real Nix's own rules
 * (ported from ~/nix/src/libstore/path.cc's `checkName`): non-empty, at
 * most 211 bytes (`StorePath::MaxNameLen`), every byte one of
 * `[0-9a-zA-Z+-._?=]`, and not literally "." or ".." or starting with
 * "-." or "-.." (the leading-dash-separated-component special case).
 * Fatal (prints an "invalid derivation name" message and exits) on a
 * violation, mirroring real Nix's own `BadStorePathName` exception. */
void ll_check_store_path_name(const char *name);

#endif /* LL_STORE_PATH_H */
