/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "ast.h"

#include <stdlib.h>
#include <string.h>

#include "compiler.h"

/* An AST is compile-time scratch: once ll_compile() walks it into an
 * IrProgram, nothing downstream reads an Ast* again. This file keeps its
 * own chunk list, freed in one shot by ll_ast_arena_reset() right after
 * each top-level ll_compile()/ll_compile_scoped() call returns (see
 * eval_file.c's call sites), instead of living in the main arena. */
#define AST_CHUNK_SIZE (1 << 20)

typedef struct AstChunk {
    struct AstChunk *next;
    size_t used;
    char data[AST_CHUNK_SIZE];
} AstChunk;

static AstChunk *g_ast_chunks = NULL;

/* Lazy-thunk-compile experiment: a second, parallel chunk list that
 * ll_ast_arena_reset() below never frees, for a deferred stub's retained
 * subtree. Gated on `ll_lazy_thunk_enabled` (compiler.h) -- an
 * unconditional form would redirect every Ast node into the never-freed
 * list regardless of whether the flag was ever set, inflating peak RSS
 * for callers not opting into the experiment (confirmed regression,
 * fixed by gating here). */
static AstChunk *g_ast_retained_chunks = NULL;
static int g_ast_redirect_retained = 0;

int ll_ast_enter_retained(void) {
    int prev = g_ast_redirect_retained;
    if (ll_lazy_thunk_enabled) g_ast_redirect_retained = 1;
    return prev;
}

void ll_ast_leave_retained(int prev_state) {
    g_ast_redirect_retained = prev_state;
}

/* Every Ast node must start zero-filled -- parser.c's plain `a: body`
 * lambda case never sets AST_LAMBDA.is_pattern/formals/has_ellipsis/
 * has_as_name explicitly, relying on it defaulting to 0/NULL;
 * AST_ATTRSET's bare `{}` and other call sites rely on the same
 * guarantee. Only `sizeof(Ast)` bytes are zeroed per node (not the whole
 * chunk), since that's all the guarantee needs to cover. */
Ast *ll_ast_new(AstKind kind) {
    size_t need = sizeof(Ast);
    AstChunk **chunks = g_ast_redirect_retained ? &g_ast_retained_chunks : &g_ast_chunks;
    if (!*chunks || (*chunks)->used + need > AST_CHUNK_SIZE) {
        AstChunk *c = malloc(sizeof(AstChunk));
        c->next = *chunks;
        c->used = 0;
        *chunks = c;
    }
    Ast *n = (Ast *)((*chunks)->data + (*chunks)->used);
    (*chunks)->used += need;
    memset(n, 0, need);
    n->kind = kind;
    return n;
}

/* Frees every chunk ever allocated by ll_ast_new -- safe to call only
 * once the caller is certain no Ast* from the chunks being freed is
 * still reachable (true right after ll_compile()/ll_compile_scoped()
 * returns, per this file's own header comment above). */
void ll_ast_arena_reset(void) {
    AstChunk *c = g_ast_chunks;
    while (c) {
        AstChunk *next = c->next;
        free(c);
        c = next;
    }
    g_ast_chunks = NULL;
}
