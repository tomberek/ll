/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef ASNIX_BYTECODE_CACHE_H
#define ASNIX_BYTECODE_CACHE_H

#include <stdint.h>

#include "asnix/ir.h"

typedef struct Ast Ast; /* ast.h's own tag -- forward-declared so this header
                          * doesn't need a full #include "ast.h" */

/* On-disk cache of compiled IrPrograms, located by resolved absolute
 * file path alone (a file's own path is still PART of what makes an
 * entry valid for it -- a compiled program can embed its own file PATH
 * directly as data, AST_CURPOS's `file` field in compiler.c -- but as
 * of M102/Finding 1, path is now the sole filename/lookup key, not
 * path+content; freshness against the CURRENT content at that path is
 * validated separately via a cheap mtime+size check, with a content-
 * hash fallback -- see bytecode_cache.c's own header-format comment and
 * asnix_bytecode_cache_lookup_fast's doc comment below for the full
 * reasoning and the exact tradeoff accepted). Two identical-content
 * files at different paths still never share an entry (different
 * filenames, since the key is the path). See bytecode.h/.c for the
 * actual IrProgram (de)serialization this wraps, and
 * docs/architecture.md's M92/M102 notes for the full design writeup
 * (iets's cache.rs is the direct model for this strategy, not for
 * asnix's own bytecode shape -- see that doc for why the two differ).
 *
 * Entirely opt-in: every function here is a no-op (always a cache miss,
 * never writes) unless the ASNIX_BYTECODE_CACHE=1 environment variable
 * is set. Any anomaly at any point (missing/unwritable directory,
 * truncated/corrupt file, bad magic/version, a deserialize failure) is
 * treated as a plain cache miss -- never fatal, never surfaced to the
 * caller beyond "use the normal compile path instead." */

/* ASNIX_BYTECODE_CACHE_STATS=1 (alongside ASNIX_BYTECODE_CACHE=1) prints
 * a one-line "N hits (F fast-path, V content-verified), M misses, K
 * stores (P% hit rate)" summary to stderr at process exit -- the cache
 * is otherwise completely silent by design (a miss, a corrupt entry,
 * and "never looked up" are all indistinguishable from the outside), so
 * this is the only way to tell whether a given run is actually
 * benefiting from it, and -- since M102 -- whether it's benefiting from
 * the CHEAP (fast) path or paying the content-hash (verify) fallback. */

/* True iff ASNIX_BYTECODE_CACHE=1 is set. Checked once and cached. */
int asnix_bytecode_cache_enabled(void);

/* M102 (Finding 1): the cache key is now (resolved path) alone for the
 * purpose of LOCATING an entry on disk -- freshness is validated
 * separately, cheaply, using mtime+size BEFORE ever reading the source
 * file. See bytecode_cache.c's own header-format comment for exactly
 * what's stored in an entry and why. This split replaces the single
 * old `asnix_bytecode_cache_lookup(path, source, source_len)` (which
 * had to read+hash the ENTIRE source file on every call, hit or miss,
 * to even compute the lookup key -- measured 1,059 source opens + 1,056
 * cache-entry opens on ffmpeg's real 1056-file closure in warm-cache
 * mode, MORE opens than the 2,444 syscalls of not caching at all) with
 * two explicit steps:
 *
 *   1. asnix_bytecode_cache_lookup_fast -- `mtime_sec`/`mtime_nsec`/
 *      `size` are whatever the caller's OWN path-resolution stat call
 *      already produced (eval_file.c's resolve_file_path, itself built
 *      on M83's follow_symlink_chain) -- no new syscall in the common
 *      direct-file-import case. Returns a ready IrProgram on a
 *      confirmed-fresh hit (recorded mtime+size match exactly) with
 *      ZERO source-file I/O at all -- just one open+read of the cache
 *      entry itself. Returns NULL otherwise, setting
 *      `*out_entry_exists` to 1 if an entry was found but didn't match
 *      (mtime/size drifted -- caller should try step 2 below) or 0 if
 *      there's no entry at all (a plain miss, nothing more to check).
 *
 *   2. asnix_bytecode_cache_lookup_verify -- only useful when step 1
 *      set `*out_entry_exists`. Needs the actual source bytes (which
 *      the caller must now read, exactly like an ordinary cache miss
 *      would anyway) to compute a real content hash and compare it
 *      against the one recorded in that same entry, disambiguating
 *      "mtime/size drifted but the content is still byte-identical"
 *      (e.g. `touch(1)`, or a checkout tool that doesn't preserve
 *      mtimes) from "genuinely different file." On a content match it
 *      also re-stores the entry with the fresh mtime/size (atomically,
 *      via the same write-temp-then-rename path asnix_bytecode_cache_store
 *      uses) so the FAST path starts working again for this path next
 *      time -- a one-time cost, not a recurring one, as long as the
 *      file doesn't keep changing mtime without changing content.
 *
 * Correctness note (mtime+size as a freshness heuristic, not a
 * cryptographic guarantee): a file rewritten with byte-identical mtime
 * AND size via unusual tooling (clock rollback, a tool that explicitly
 * preserves both) could in principle produce a stale fast-path hit --
 * the same tradeoff ccache's default mode and Nix's own eval cache both
 * accept and document. Mitigated, not eliminated, by also checking
 * `size` (not just mtime) and by the verify path's real content-hash
 * fallback catching the overwhelmingly common case (mtime changes on
 * any normal write); a maximally-paranoid alternative (hash the content
 * on every single lookup, defeating the whole point of this fix) was
 * rejected as not worth it for an opt-in, off-by-default developer-
 * convenience cache -- see bytecode_cache.c's own header-format comment
 * for the full reasoning. */
IrProgram *asnix_bytecode_cache_lookup_fast(const char *resolved_path, int64_t mtime_sec, int64_t mtime_nsec,
                                             int64_t size, int *out_entry_exists);
IrProgram *asnix_bytecode_cache_lookup_verify(const char *resolved_path, const char *source, size_t source_len,
                                               int64_t mtime_sec, int64_t mtime_nsec, int64_t size);

/* Writes `prog` to the cache under the same (path-hashed) key the two
 * lookup functions above would look it up with, recording `mtime_sec`/
 * `mtime_nsec`/`size` (the caller's own already-available stat info --
 * see asnix_bytecode_cache_lookup_fast's comment) and a fresh content
 * hash of `source`/`source_len` into the entry's header, so later runs
 * can validate freshness via either path. No-op if the cache is
 * disabled. Best-effort: any failure (serialize refuses an unsupported
 * construct, disk error, lock contention) is silently ignored -- a
 * missing/stale cache entry is always safe, just slower, never wrong. */
void asnix_bytecode_cache_store(const char *resolved_path, const char *source, size_t source_len, int64_t mtime_sec,
                                 int64_t mtime_nsec, int64_t size, IrProgram *prog);

/* M92: opt-in background prefetch -- ASNIX_BYTECODE_PRODUCER=1 (on top
 * of ASNIX_BYTECODE_CACHE=1, which it requires) scans a freshly parsed
 * AST for statically-literal `import "<path>"`-shaped call targets not
 * already cached, and fork()s one child process per discovery to
 * compile-and-cache each in the background while the parent continues
 * evaluating. Deliberately a forked PROCESS, not a pthread: the global
 * arena (asnix_arena, src/asm/arena.S) and symbol table (symtab.c) have
 * no synchronization at all, and retrofitting that safely was judged
 * out of scope for this pass (see docs/architecture.md's M92 notes) --
 * fork()'s copy-on-write address-space duplication gives the same
 * "separate core, separate L1/L2 pointer-chasing, dedicated parser"
 * property the threading hypothesis wants, with zero risk of the parent
 * and child's compile-time allocators racing, since after fork() they
 * are simply two independent processes that happen to share no mutable
 * state at all. The child writes its result to the same on-disk cache
 * asnix_bytecode_cache_store would, then exits; the parent only ever
 * reads that cache through the ordinary asnix_bytecode_cache_lookup
 * path, so there is no cross-process pointer handoff of any kind.
 * No-op (does nothing, returns immediately) unless both env vars are
 * set. Safe to call on every parsed file; internally no-ops on anything
 * that isn't a plain, already-cached-checked, statically-resolvable
 * import target. */
void asnix_bytecode_prefetch_imports(Ast *root, const char *base_dir);

/* Waits for (reaps) any outstanding prefetch children this process
 * started whose target path matches `resolved_path` -- called right
 * before a synchronous cache lookup for that same path, so a producer
 * that's already most of the way done gets a chance to finish and
 * populate the cache before the consumer falls back to compiling it
 * itself redundantly. Bounded wait (a short timeout, not indefinite) --
 * if the child hasn't finished yet, returns immediately and the caller
 * proceeds with its own normal cache-miss fallback exactly as if no
 * prefetch had ever been attempted. No-op if prefetching is disabled or
 * no outstanding child matches. */
void asnix_bytecode_prefetch_wait(const char *resolved_path);

#endif /* ASNIX_BYTECODE_CACHE_H */
