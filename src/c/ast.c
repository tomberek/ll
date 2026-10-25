/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "ast.h"

#include <stdlib.h>
#include <string.h>

/* perf: Ast nodes used to come from the main never-free bump arena, same
 * as every runtime value -- but an AST is pure compile-time scratch:
 * once asnix_compile() walks it into an IrProgram, nothing downstream
 * ever reads an Ast* again (AstBinding.name/AstFormal.name are raw
 * pointers into the source TEXT buffer, not into Ast nodes, and
 * AST_CURPOS bakes its line/col into the IR as plain integers at compile
 * time). So this file keeps its own chunk list, freed in one shot by
 * asnix_ast_arena_reset() right after each top-level asnix_compile()/
 * asnix_compile_scoped() call returns (see eval_file.c's call sites),
 * instead of living in the main arena forever. Measured this session:
 * Ast nodes were ~17% of asnix's total memory footprint on a real
 * nixpkgs eval, entirely reclaimable since nothing needs them past
 * compile time. */
#define AST_CHUNK_SIZE (1 << 20) /* 1 MiB per chunk -- same order as the main arena's 2 MiB */

typedef struct AstChunk {
    struct AstChunk *next;
    size_t used;
    char data[AST_CHUNK_SIZE];
} AstChunk;

static AstChunk *g_ast_chunks = NULL;

/* perf (M128): this chunk used to be `calloc`'d whole (1 MiB) instead of
 * `malloc`'d -- correctness NEEDS every Ast node to start zero-filled
 * (confirmed real: parser.c's plain `a: body` lambda case never sets
 * AST_LAMBDA.is_pattern/formals/has_ellipsis/has_as_name explicitly,
 * relying on it defaulting to 0/NULL; AST_ATTRSET's bare `{}` and
 * several other call sites lean on the same guarantee for fields they
 * don't bother re-stating), but that guarantee only has to hold for the
 * bytes a node ACTUALLY occupies, not the whole 1 MiB chunk up front --
 * so it's cheaper to zero exactly `sizeof(Ast)` bytes once per node
 * (below) than the entire chunk once per chunk.
 *
 * Why this mattered in practice, found via `perf record`/`perf report`
 * on an isolated lex+parse+compile-only harness over the real 1057-file
 * nixpkgs corpus `d.nix` touches: `calloc`'s own zero-fill showed up as a
 * genuine `__memset_avx2_unaligned_erms` hot symbol (~5.7% of that
 * harness's total cycles), NOT eliminated by `calloc`'s usual "mmap
 * already gives zero pages, skip the memset" fast path -- because
 * `asnix_ast_arena_reset()` frees every chunk right after each FILE's
 * compile (by design, see this file's own header comment: Ast nodes are
 * ~17% of peak memory and fully reclaimable), so this process allocates-
 * then-frees a ~1 MiB chunk roughly once per imported file (1057 times
 * for this corpus). glibc's malloc dynamically RAISES its own mmap
 * threshold every time a chunk at or above the current threshold is
 * freed (to stop thrashing mmap/munmap for a workload that repeats the
 * same large size) -- confirmed via glibc's own `mallopt(M_MMAP_
 * THRESHOLD)` documentation of this dynamic-adjustment behavior -- so
 * after the first one or two ~1 MiB frees, the threshold climbs above
 * 1 MiB and every SUBSEQUENT same-sized chunk is served from the
 * ordinary heap instead of a fresh mmap, where `calloc` can no longer
 * assume pre-zeroed pages and must really `memset` the whole thing.
 * Worse, the overwhelming majority of nixpkgs's 1057 imported files are
 * far smaller than 1 MiB of AST, so most of that `memset` work was spent
 * zeroing bytes the file's own parse never even touched.
 *
 * `malloc`ing the chunk instead sidesteps the dynamic-threshold problem
 * entirely (malloc makes no zero-fill promise either way, so there's
 * nothing to skip or fall back to), and the new per-node `memset` is
 * proportional to the file's ACTUAL node count, not the fixed chunk
 * size -- strictly less total zeroing than before for every file that
 * doesn't fill a whole chunk (the common case), and no worse for one
 * that does. */
Ast *asnix_ast_new(AstKind kind) {
    size_t need = sizeof(Ast);
    if (!g_ast_chunks || g_ast_chunks->used + need > AST_CHUNK_SIZE) {
        AstChunk *c = malloc(sizeof(AstChunk));
        c->next = g_ast_chunks;
        c->used = 0;
        g_ast_chunks = c;
    }
    Ast *n = (Ast *)(g_ast_chunks->data + g_ast_chunks->used);
    g_ast_chunks->used += need;
    memset(n, 0, need);
    n->kind = kind;
    return n;
}

/* Frees every chunk ever allocated by asnix_ast_new -- safe to call only
 * once the caller is certain no Ast* from the chunks being freed is
 * still reachable (true right after asnix_compile()/asnix_compile_scoped()
 * returns, per this file's own header comment above). */
void asnix_ast_arena_reset(void) {
    AstChunk *c = g_ast_chunks;
    while (c) {
        AstChunk *next = c->next;
        free(c);
        c = next;
    }
    g_ast_chunks = NULL;
}
