/* asnix's on-disk bytecode cache (bytecode.c/bytecode_cache.c) serializes
 * IrOp.handler as a *linked* asm address and is opt-in (off unless
 * ASNIX_BYTECODE_CACHE=1), so it was not vendored -- the LLVM backend needs
 * a different caching mechanism keyed to compiled object code, not to
 * asnix_handler_table addresses (see soft-wishing-engelbart.md's M9+ note
 * on deferred AOT/object-code caching). This stub keeps eval_file.c's
 * cache-lookup call sites linkable while always reporting "disabled",
 * identical in effect to running real asnix with the env var unset. */
#include <stddef.h>
#include <stdint.h>

#include "ast.h"
#include "asnix/ir.h"

int asnix_bytecode_cache_enabled(void) {
    return 0;
}

IrProgram *asnix_bytecode_cache_lookup_fast(const char *resolved_path, int64_t mtime_sec, int64_t mtime_nsec,
                                             int64_t size, int *out_entry_exists) {
    (void)resolved_path;
    (void)mtime_sec;
    (void)mtime_nsec;
    (void)size;
    *out_entry_exists = 0;
    return NULL;
}

IrProgram *asnix_bytecode_cache_lookup_verify(const char *resolved_path, const char *source, size_t source_len,
                                               int64_t mtime_sec, int64_t mtime_nsec, int64_t size) {
    (void)resolved_path;
    (void)source;
    (void)source_len;
    (void)mtime_sec;
    (void)mtime_nsec;
    (void)size;
    return NULL;
}

void asnix_bytecode_cache_store(const char *resolved_path, const char *source, size_t source_len, int64_t mtime_sec,
                                 int64_t mtime_nsec, int64_t size, IrProgram *prog) {
    (void)resolved_path;
    (void)source;
    (void)source_len;
    (void)mtime_sec;
    (void)mtime_nsec;
    (void)size;
    (void)prog;
}

void asnix_bytecode_prefetch_imports(Ast *root, const char *base_dir) {
    (void)root;
    (void)base_dir;
}

void asnix_bytecode_prefetch_wait(const char *resolved_path) {
    (void)resolved_path;
}
