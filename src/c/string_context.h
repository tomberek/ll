/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_STRING_CONTEXT_H
#define LL_STRING_CONTEXT_H

#include <stddef.h>

#include "ll/value.h"

/*
 * Real Nix string context -- an invisible dependency-provenance set
 * attached to a string *value*, surviving concatenation/coercion. This
 * is load-bearing for hash parity with real Nix, not just an
 * introspection nicety: a derivation reference can reach a plain opaque
 * string via interpolation (e.g. `shell = "${stage0.bash}/bin/bash";`)
 * long before any structural collector that walks a derivation attrset/
 * list/Path ever sees it -- once a reference becomes a STRING via
 * interpolation, there's no structure left to walk, so the dependency
 * has to be tracked out-of-band instead.
 *
 * Context is attached via a SIDE TABLE keyed by the string Value's own
 * arena pointer, NOT baked into the string heap layout (unlike Path/
 * Float's marker-byte trick, include/ll/heap.h): ordinary string reads
 * (every asm op: equality, printing, hashing, OP_STR_CONCAT's own
 * byte-copying) stay COMPLETELY untouched, since they never need to
 * know a context exists. Only the specific C-level functions that
 * *construct* a new string from existing string/attrs/list inputs need
 * to query-and-propagate.
 */

typedef struct {
    char *drv_path;
    char *output_name;
} LlCtxBuiltRef;

typedef struct {
    LlCtxBuiltRef *drvs;
    size_t ndrvs, drvs_cap;
    /* Open-addressing dedup indexes for ll_ctx_add_built/
     * ll_ctx_add_opaque/ll_ctx_add_deep (string_context.c) -- parallel
     * to drvs/srcs/deep below, giving O(1) amortized dedup-on-insert
     * instead of a linear strcmp scan. */
    size_t *drvs_index;
    size_t drvs_index_cap;
    char **srcs;
    size_t nsrcs, srcs_cap;
    size_t *srcs_index;
    size_t srcs_index_cap;
    /* Real Nix's third context-element kind, `NixStringContextElem::
     * DrvDeep` -- "this string refers to the WHOLE closure of this
     * derivation (all its outputs), not just one named output" -- the
     * kind attached to a derivation's own `.drvPath` string (see
     * derivation.c's ll_prim_derivation) and produced by `builtins.
     * addDrvOutputDependencies`. Exposed at the language level by
     * `getContext` as `{ allOutputs = true; }` (builtins_m54.c) --
     * distinct from `drvs` above (a specific NAMED output, `{ outputs =
     * [...]; }`) and from `srcs` (a plain store-path reference with no
     * derivation relationship at all, `{ path = true; }`). Deliberately
     * a separate bare `char *` array, not a 3rd field on LlCtxBuiltRef,
     * since a DrvDeep element has no output name at all (unlike Built)
     * -- reusing that struct with a NULL/sentinel output_name would
     * make every existing `ndrvs`-only reader (derivation.c's
     * ll_collect_input_refs) responsible for skipping it, which this
     * keeps them oblivious to instead. */
    char **deep;
    size_t ndeep, deep_cap;
    size_t *deep_index;
    size_t deep_index_cap;
} LlStringContext;

void ll_ctx_init(LlStringContext *ctx);
void ll_ctx_add_built(LlStringContext *ctx, const char *drv_path, const char *output_name);
void ll_ctx_add_opaque(LlStringContext *ctx, const char *path);
/* See LlStringContext's own `deep` field comment just above. */
void ll_ctx_add_deep(LlStringContext *ctx, const char *drv_path);
int ll_ctx_is_empty(const LlStringContext *ctx);
/* dst |= src (deduped) -- a no-op if src is NULL or empty. */
void ll_ctx_union_into(LlStringContext *dst, const LlStringContext *src);

/* Attaches a deep copy of `ctx` to `v` (a String Value, i.e.
 * LL_TAG_PTR, not a Path/Float) in the process-lifetime side table --
 * a no-op if `ctx` is NULL or empty (the overwhelmingly common case:
 * every plain-literal/arithmetic/non-derivation string never gets an
 * entry at all, so lookups for them stay a fast "not found"). */
void ll_string_attach_context(Value v, const LlStringContext *ctx);

/* NULL if `v` has no attached context. The returned pointer is owned by
 * the side table (process lifetime, same memory model as the rest of
 * this codebase) -- never free it. */
const LlStringContext *ll_string_context_of(Value v);

#endif /* LL_STRING_CONTEXT_H */
