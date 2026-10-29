/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_BYTECODE_CACHE_H
#define LL_BYTECODE_CACHE_H

#include <stdint.h>

#include "ll/ir.h"

typedef struct Ast Ast;

/* Entirely opt-in: every function here is a no-op (always a cache miss,
 * never writes) unless the LL_BYTECODE_CACHE=1 environment variable
 * is set. Any anomaly at any point (missing/unwritable directory,
 * truncated/corrupt file, bad magic/version, a deserialize failure) is
 * treated as a plain cache miss -- never fatal, never surfaced to the
 * caller beyond "use the normal compile path instead." */

int ll_bytecode_cache_enabled(void);

/* The cache key is the resolved path alone for the purpose of LOCATING
 * an entry on disk -- freshness is validated separately, cheaply, using
 * mtime+size BEFORE ever reading the source file:
 *
 *   1. ll_bytecode_cache_lookup_fast -- `mtime_sec`/`mtime_nsec`/
 *      `size` are whatever the caller's OWN path-resolution stat call
 *      already produced -- no new syscall in the common direct-file-
 *      import case. Returns a ready IrProgram on a confirmed-fresh hit
 *      with ZERO source-file I/O at all. Returns NULL otherwise, setting
 *      `*out_entry_exists` to 1 if an entry was found but didn't match
 *      (mtime/size drifted -- caller should try step 2 below) or 0 if
 *      there's no entry at all.
 *
 *   2. ll_bytecode_cache_lookup_verify -- only useful when step 1
 *      set `*out_entry_exists`. Needs the actual source bytes to compute
 *      a real content hash and compare it against the one recorded in
 *      that same entry, disambiguating "mtime/size drifted but the
 *      content is still byte-identical" from "genuinely different
 *      file." On a content match it also re-stores the entry with the
 *      fresh mtime/size so the FAST path starts working again for this
 *      path next time. */
IrProgram *ll_bytecode_cache_lookup_fast(const char *resolved_path, int64_t mtime_sec, int64_t mtime_nsec,
                                             int64_t size, int *out_entry_exists);
IrProgram *ll_bytecode_cache_lookup_verify(const char *resolved_path, const char *source, size_t source_len,
                                               int64_t mtime_sec, int64_t mtime_nsec, int64_t size);

void ll_bytecode_cache_store(const char *resolved_path, const char *source, size_t source_len, int64_t mtime_sec,
                                 int64_t mtime_nsec, int64_t size, IrProgram *prog);

/* Opt-in background prefetch -- LL_BYTECODE_PRODUCER=1 (on top of
 * LL_BYTECODE_CACHE=1, which it requires) scans a freshly parsed AST for
 * statically-literal `import "<path>"`-shaped call targets not already
 * cached, and fork()s one child process per discovery to compile-and-
 * cache each in the background while the parent continues evaluating.
 * Deliberately a forked PROCESS, not a pthread: the global arena
 * (ll_arena, src/asm/arena.S) and symbol table (symtab.c) have no
 * synchronization at all, so fork()'s copy-on-write address-space
 * duplication is used instead to give each process its own allocator
 * with zero risk of the parent and child racing. The child writes its
 * result to the same on-disk cache ll_bytecode_cache_store would, then
 * exits; the parent only ever reads that cache through the ordinary
 * lookup path, so there is no cross-process pointer handoff of any
 * kind. No-op unless both env vars are set. */
void ll_bytecode_prefetch_imports(Ast *root, const char *base_dir);

/* Waits for (reaps) any outstanding prefetch children this process
 * started whose target path matches `resolved_path` -- called right
 * before a synchronous cache lookup for that same path, so a producer
 * that's already most of the way done gets a chance to finish and
 * populate the cache before the consumer falls back to compiling it
 * itself redundantly. Bounded wait, not indefinite. No-op if prefetching
 * is disabled or no outstanding child matches. */
void ll_bytecode_prefetch_wait(const char *resolved_path);

#endif /* LL_BYTECODE_CACHE_H */
