/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_STRING_CONTEXT_H
#define LL_STRING_CONTEXT_H

#include <stddef.h>

#include "ll/value.h"

/*
 * M76: real Nix string context -- an invisible dependency-provenance set
 * attached to a string *value*, surviving concatenation/coercion. Found
 * load-bearing (not just a nice-to-have introspection feature) chasing
 * d.nix past M74/M75: nixpkgs sets `builder = stdenvShell;`, which
 * traces back to `shell = "${stage0.bash}/bin/bash";` -- a STRING
 * INTERPOLATION of a derivation reference, concatenated with a literal
 * suffix, evaluated to a plain opaque string long before `derivation
 * {...}`'s primop ever sees it. M74's structural collector
 * (ll_collect_input_refs, derivation.c) walks an already-forced
 * Value looking for a bare derivation attrset/list/Path -- it is blind
 * to this by construction, since once a reference becomes a STRING via
 * interpolation, there's no structure left to walk. This is the same
 * feature M61 (and implicitly M45/M54/M60) already declined once, for
 * being disproportionate to its own user-facing payoff
 * (getContext/hasContext/addDrvOutputDependencies) -- revealed here to
 * also be load-bearing for hash parity, not just introspection.
 *
 * Key simplification vs. that earlier, declined scope: that estimate
 * (~101 call sites, a new tagged Value kind touching the asm layer)
 * assumed context would be baked into the STRING HEAP LAYOUT itself
 * (like Path/Float's marker-byte trick, include/ll/heap.h). Instead,
 * this attaches context via a SIDE TABLE keyed by the string Value's
 * own arena pointer (a linear-scan array, same convention as
 * eval_file.c's import cache / derivation.c's masked-hash cache) --
 * ordinary string reads (every asm op: equality, printing, hashing,
 * OP_STR_CONCAT's own byte-copying) stay COMPLETELY untouched, since
 * they never need to know a context exists. Only the specific C-level
 * functions that *construct* a new string from existing string/attrs/
 * list inputs need to query-and-propagate.
 *
 * Reuses exactly M74's own BuiltRef/InputRefSet vocabulary (dedup-on-
 * insert growable arrays) -- this type replaces that derivation.c-local
 * struct, moved here since context needs to be readable from
 * value_util.c, builtins_m54.c, AND derivation.c.
 */

typedef struct {
    char *drv_path;
    char *output_name;
} LlCtxBuiltRef;

typedef struct {
    LlCtxBuiltRef *drvs;
    size_t ndrvs, drvs_cap;
    /* perf: open-addressing dedup indexes for ll_ctx_add_built/
     * ll_ctx_add_opaque/ll_ctx_add_deep (string_context.c) --
     * parallel to drvs/srcs/deep below. Mirrors derivation.c's
     * InputRefSet (drvs_index/srcs_index, same file's dcc_hash_str) --
     * see string_context.c for why a per-add linear strcmp scan here
     * was a measured O(n^2) anti-pattern. */
    size_t *drvs_index;
    size_t drvs_index_cap;
    char **srcs;
    size_t nsrcs, srcs_cap;
    size_t *srcs_index;
    size_t srcs_index_cap;
    /* M80: real Nix's third context-element kind, `NixStringContextElem::
     * DrvDeep` -- "this string refers to the WHOLE closure of this
     * derivation (all its outputs), not just one named output" -- the
     * kind attached to a derivation's own `.drvPath` string (see
     * derivation.c's ll_prim_derivation) and produced by `builtins.
     * addDrvOutputDependencies`. Exposed at the language level by
     * `getContext` as `{ allOutputs = true; }` (builtins_m54.c) --
     * distinct from `drvs` above (a specific NAMED output, `{ outputs =
     * [...]; }`) and from `srcs` (a plain store-path reference with no
     * derivation relationship at all, `{ path = true; }`). Deliberately
     * a separate bare `char *` array, not a 3rd field on
     * LlCtxBuiltRef, since a DrvDeep element has no output name at
     * all (unlike Built) -- reusing that struct with a NULL/sentinel
     * output_name would make every existing `ndrvs`-only reader
     * (derivation.c's ll_collect_input_refs) responsible for skipping
     * it, which this keeps them oblivious to instead (that file is
     * deliberately untouched by this milestone -- a DrvDeep-tagged
     * string used directly as a derivation attribute's VALUE still
     * collects zero input refs, exactly like before this milestone,
     * when `.drvPath` carried no context at all; full "deep" closure
     * dependency propagation is a separate, substantially larger
     * feature -- see M80's own scope notes in docs/architecture.md). */
    char **deep;
    size_t ndeep, deep_cap;
    size_t *deep_index;
    size_t deep_index_cap;
} LlStringContext;

void ll_ctx_init(LlStringContext *ctx);
void ll_ctx_add_built(LlStringContext *ctx, const char *drv_path, const char *output_name);
void ll_ctx_add_opaque(LlStringContext *ctx, const char *path);
/* M80: see LlStringContext's own `deep` field comment just above. */
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
