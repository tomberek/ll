/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_COMPILER_H
#define LL_COMPILER_H

#include <stddef.h>

#include "ast.h"
#include "ll/ir.h"

/* Compiles an AST produced by ll_parse() into a linked (handler
 * addresses already resolved) IrProgram ready for ll_eval(). Name
 * resolution happens inline during this walk (no separate annotation
 * pass) -- see src/c/compiler.c for why that's sufficient at M4's scope.
 *
 * `base_dir` is the absolute directory of the .nix file being compiled
 * (no trailing slash) -- relative path literals (AST_PATH, e.g.
 * `./foo.nix`) resolve against it, matching real Nix's rule that a path
 * literal resolves relative to the directory of the file it's *written
 * in*, not the process's cwd or the importing file's directory (see
 * src/c/eval_file.c, which computes this once per file and is the only
 * other caller of ll_compile besides this file's own tests).
 *
 * `file_path` (M82): the resolved absolute path of the .nix file being
 * compiled (not just its directory) -- used ONLY for `__curPos`'s `file`
 * field (AST_CURPOS in compile_expr), so a `__curPos` token inside an
 * IMPORTED file reports that file's own path, not the outermost file
 * asnix was invoked on (the old M80 approximation). May be NULL (falls
 * back to `ll_eval_top_level_file()`, matching the old behavior
 * exactly) -- every real caller passes it, NULL only covers this file's
 * own unit tests, which don't exercise `__curPos`. */
IrProgram *ll_compile(Ast *root, const char *base_dir, const char *file_path);

/* M54: `builtins.scopedImport`'s compile-time half -- same as
 * ll_compile above, but with one extra named (lexical, not `with`-
 * style fallback) ScopeFrame of `extra_count` names (`extra_names`/
 * `extra_lens`) spliced in between the ordinary root `builtins` frame
 * and `root`'s own top-level expression, so each of those names resolves
 * (and takes priority over any bare global/builtins.X of the same name)
 * like any other variable reference. See compiler.c's own comment on
 * this function and eval_file.c's ll_eval_file_scoped (the only
 * caller) for the matching runtime half. */
IrProgram *ll_compile_scoped(Ast *root, const char *base_dir, const char *file_path,
                                 const char **extra_names, size_t *extra_lens, size_t extra_count);

/* Lazy-thunk-compile experiment, ported from asnix (docs/lazy-thunk-
 * compile-proposal.md): 0 by default (every binding still compiles
 * eagerly, byte-identical to before this experiment existed); set once
 * from main() via `getenv("LL_LAZY_THUNK")`. When set,
 * `compile_expr`'s AST_ATTRSET case defers an ordinary field's value to
 * first-force instead of compiling it immediately, AND eval_file.c's
 * `free(src)` call sites skip freeing the source buffer (hazard #3's
 * simple-but-correct fix: a deferred subtree's Ast string fields still
 * alias `src`, so it must outlive the file's own compile call for as
 * long as any of its stubs might still fire -- matching asnix's own
 * tradeoff exactly). */
extern int ll_lazy_thunk_enabled;

/* C bridge for OP_LAZY_THUNK_BODY's handler (src/llvm/ops/
 * op_lazy_thunk_body.c) -- takes the opaque LazyThunkDescriptor* pool
 * entry, compiles its deferred subtree for real, mutates the shared
 * IrProgram in place, and returns it (same pointer, now carrying real
 * ops/pool). Takes `void *` for the same opaque-pointer reason
 * ib_mk_lazy_thunk_stub does (irbuild.h) -- LazyThunkDescriptor is
 * defined in compiler.c only, nothing outside it needs the layout. */
IrProgram *ll_compile_lazy_thunk_body(void *desc);

/* ---------------------------------------------------------------------
 * Bytecode-cache + lazy-thunk reconciliation (bytecode.c's
 * PTAG_OP_LAZY_MARKER): LazyThunkDescriptor/ScopeFrame stay opaque to
 * every other file (see ll_compile_lazy_thunk_body's own comment
 * above) -- these accessors are the ONLY sanctioned way to read one from
 * bytecode.c, and every one returns plain data (an int, a byte string,
 * or another opaque handle to walk further), never a raw Ast*. The
 * "_scope"/"_frame_*" family lets bytecode.c walk a LIVE descriptor's
 * ScopeFrame chain to SERIALIZE it; the "_push_frame"/"_make_cached_"/
 * "_set_self" family lets it build an equivalent descriptor back up from
 * deserialized (count, is_with, has_overrides, name) tuples, without
 * ever needing ScopeFrame's or LazyThunkDescriptor's own struct layout.
 * --------------------------------------------------------------------- */

/* 0 if `desc`'s deferred value has no real, verbatim, standalone-
 * reparseable source span (a compiler-synthesized value, e.g. the
 * hidden AST_SELECT an `inherit (expr) a b;` clause's desugaring can
 * produce -- see parser.c's parse_binding_stmt_inner comment) -- the
 * caller must refuse to cache this program in that case, exactly like
 * bytecode.c's program_op_count did unconditionally before this
 * feature existed. Otherwise writes the real byte offset to `*out_pos`
 * and returns 1. */
int ll_lazy_thunk_descriptor_src_pos(void *desc, unsigned int *out_pos);
const char *ll_lazy_thunk_descriptor_file_path(void *desc);
const char *ll_lazy_thunk_descriptor_base_dir(void *desc);
/* Opaque ScopeFrame* handle, or NULL at/past the root. */
void *ll_lazy_thunk_descriptor_scope(void *desc);
void *ll_scope_frame_up(void *frame);
int ll_scope_frame_count(void *frame);
int ll_scope_frame_is_with(void *frame);
int ll_scope_frame_has_overrides(void *frame);
const char *ll_scope_frame_name(void *frame, int i, size_t *out_len);

/* Pushes one more (innermost) ScopeFrame onto `up_chain` (NULL for the
 * first/outermost call), deep-copying `names`/`name_lens` into fresh,
 * never-freed storage (same convention scope_frame_persist already
 * uses for the live path -- see that function's own comment) since the
 * caller's own `names[i]` here typically point into a bytecode-cache
 * mmap that's unmapped right after deserialization returns. Returns the
 * new (opaque) ScopeFrame* handle, to pass back in as `up_chain` for the
 * next, more-deeply-nested frame. */
void *ll_lazy_scope_push_frame(void *up_chain, int count, int is_with, int has_overrides,
                                   const char **names, const size_t *name_lens);

/* Builds a cache-load LazyThunkDescriptor: `self` starts unset (the
 * stub IrProgram it belongs to doesn't exist yet at this point -- same
 * chicken-and-egg order the live path's own emit_attrset_field_value
 * resolves via its own two-step `ib_finish` then `desc->self = ...`) --
 * call ll_lazy_thunk_descriptor_set_self once the caller has built
 * that one-op program. `file_path`/`base_dir`/`scope_chain` are taken by
 * pointer, not copied again -- the caller must have already allocated
 * them in fresh, never-freed storage (mirroring every other "own it
 * forever" convention this experiment already uses). Returns an opaque
 * `void *` suitable for ib_mk_lazy_thunk_stub's own pool slot. */
void *ll_make_cached_lazy_thunk_descriptor(unsigned int reparse_pos, const char *file_path,
                                               const char *base_dir, void *scope_chain);
void ll_lazy_thunk_descriptor_set_self(void *desc, IrProgram *self);

#endif /* LL_COMPILER_H */
