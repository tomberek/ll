/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef ASNIX_TEST_IRBUILD_H
#define ASNIX_TEST_IRBUILD_H

#include <stddef.h>
#include <stdint.h>
#include "asnix/ir.h"

/* IR-stream builder shared by the M3 test fixtures (tests/ir) and the real
 * M4 compiler (src/c/compiler.c) -- "append an IrOp, track patch sites" is
 * the same API either way. */

typedef struct {
    IrOp *ops;
    size_t len, cap;
    int64_t *pool;
    size_t pool_len, pool_cap;
#ifdef ASNIX_IRDUMP_LINE_TRACKING
    /* Source-position debug tracking: parallel to `ops`, one entry per
     * emitted op. Compiled into this struct ONLY in irdump's own variant
     * build (see Makefile) -- a real production `asnix`/`libasnix.a`
     * build never defines ASNIX_IRDUMP_LINE_TRACKING, so IrBuilder there
     * is byte-identical in layout to before this feature existed. */
    uint32_t *lines;
    size_t lines_cap;
#endif
} IrBuilder;

IrBuilder *ib_new(void);
size_t ib_len(IrBuilder *b);

#ifdef ASNIX_IRDUMP_LINE_TRACKING
/* Debug-only source-line tracking for irdump's "which Nix construct fired
 * this optimizer pass" feature. Compiled in ONLY for irdump's own variant
 * build of these files (ast.c/parser.c/compiler.c/irbuild.c/irpass.c --
 * see Makefile's IRDUMP_LINE_TRACKING_SRCS); a real production eval links
 * against the plain (undefined-macro) objects and never sees any of this
 * code at all -- not a runtime-disabled flag, genuinely absent from the
 * compiled instructions. Measured directly: even a runtime-flag-gated
 * version of this (checked but defaulted off) cost a real, reproducible
 * instruction regression on the real d.nix/ffmpeg eval -- not from any
 * one obvious hot branch, but diffusely enough (parser.c's 46 call sites,
 * the IrBuilder struct's own size, ib_emit's extra field write) that a
 * true compile-time exclusion was the only approach that measured as
 * genuinely free. */
void ib_enable_line_tracking(void);
void ib_set_current_src_pos(uint32_t pos);

/* Looks up the finished `lines` array a given IrProgram's builder recorded
 * (NULL if line tracking was off, or if `prog` is unrecognized) -- a
 * pointer-identity side table, not a field on IrProgram itself, so a real
 * eval's IrProgram struct (hundreds of thousands of long-lived instances
 * per real eval) never carries this debug-only pointer's permanent 8-byte
 * cost even in the irdump variant build. irdump.c is the only real
 * caller. */
const uint32_t *asnix_ib_lookup_program_lines(const IrProgram *prog);
#else
/* Plain production build: these become unconditional no-ops so compiler.c
 * (shared source between both build variants) doesn't need its own
 * #ifdef around the one call site that uses them -- see compiler.c's
 * compile_expr. `static inline`, defined here so they fully disappear
 * (no call, no symbol) rather than becoming a real but empty function
 * call. */
static inline void ib_enable_line_tracking(void) {}
static inline void ib_set_current_src_pos(uint32_t pos) { (void)pos; }
#endif

size_t ib_push_int(IrBuilder *b, int64_t v);
size_t ib_push_true(IrBuilder *b);
size_t ib_push_false(IrBuilder *b);
size_t ib_push_null(IrBuilder *b);
size_t ib_var_ref(IrBuilder *b, int64_t depth, int64_t slot);
/* M92: see OP_VAR_REF_CELL's own comment (ir.h) -- same depth/slot
 * addressing as ib_var_ref, but pushes a Cell aliasing the slot instead
 * of forcing+pushing its value. */
size_t ib_var_ref_cell(IrBuilder *b, int64_t depth, int64_t slot);
/* M92: see OP_VAR_REF_RAW's own comment (ir.h) -- same depth/slot
 * addressing as ib_var_ref, but pushes the slot's raw content with NO
 * forcing at all. */
size_t ib_var_ref_raw(IrBuilder *b, int64_t depth, int64_t slot);
size_t ib_add(IrBuilder *b);
size_t ib_sub(IrBuilder *b);
size_t ib_mul(IrBuilder *b);
size_t ib_if(IrBuilder *b);   /* emits with a placeholder delta; patch via ib_patch_delta */
size_t ib_jmp(IrBuilder *b);  /* ditto */
void ib_patch_delta(IrBuilder *b, size_t site, size_t target_idx);
size_t ib_let(IrBuilder *b, int64_t nslots);
/* body_prog must come from this same translation's ib_finish() -- its
 * `pool` travels with the resulting closure (M100: via ASNIX_CLOSURE_OFF_PROG
 * in ir.h, a single IrProgram* rather than separate body/pool fields) so a
 * lambda body containing its own nested OP_LAMBDA resolves against the
 * right constant pool at call time, not the caller's.
 * `formals_raw`: this closure's pre-built `builtins.functionArgs` result
 * Value.raw (M37) -- the caller must build it (a real `{name:
 * hasDefault}` attrset for a lambda-pattern, or any empty-attrset Value
 * otherwise) and pass it through here; see ASNIX_CLOSURE_OFF_FORMALS,
 * ir.h. */
size_t ib_lambda(IrBuilder *b, int64_t arity, IrProgram *body_prog, int64_t formals_raw);
size_t ib_call(IrBuilder *b, int64_t argc);
/* ll-only, no asnix equivalent: see ir.h's OP_CALL_CHAIN comment. */
size_t ib_call_chain(IrBuilder *b, int64_t n);
size_t ib_ret(IrBuilder *b);
size_t ib_halt(IrBuilder *b);

/* M5: pushes a fully-formed Value.raw (e.g. a string literal) stashed in
 * the constant pool. */
size_t ib_push_const(IrBuilder *b, int64_t raw_value);
size_t ib_mk_list(IrBuilder *b, int64_t count);
/* sorted_syms must already be sorted ascending -- the compiler sorts
 * bindings by symbol id and pushes their values in that same order before
 * emitting this, so OP_MK_ATTRS's reverse-pop fill lines each value up
 * with the right symbol (see op_mk_attrs.S). */
size_t ib_mk_attrs(IrBuilder *b, int64_t count, const uint32_t *sorted_syms);
size_t ib_select(IrBuilder *b, uint32_t sym);

/* `expr.${nameExpr}`: pops the name Value then the select target (both
 * already pushed by the compiler), looks the name up at runtime -- see
 * OP_SELECT_DYNAMIC's own comment (ir.h) and op_select_dynamic.S. */
size_t ib_select_dynamic(IrBuilder *b);

/* M6: pushes a thunk wrapping body_prog (compiled like a lambda body,
 * ending in OP_RET) captured over the *current* Env at the point this op
 * runs -- same pool-travels-with-it rationale as ib_lambda. */
size_t ib_mk_thunk(IrBuilder *b, IrProgram *body_prog);

/* Recursion support (letrec / rec attrsets): allocate a new Env frame with
 * `nslots` slots and install it as the current ENV (OP_FRAME_ALLOC), store
 * the value on top of VALSTACK into slot `slot` of the current ENV
 * (OP_FRAME_SET), and restore ENV to its parent (OP_FRAME_EXIT) -- see
 * ir.h for the full rationale. */
size_t ib_frame_alloc(IrBuilder *b, int64_t nslots);
size_t ib_frame_set(IrBuilder *b, int64_t slot);
size_t ib_frame_get(IrBuilder *b, int64_t slot);
size_t ib_frame_exit(IrBuilder *b);

/* `rec { ... __overrides = {...}; ... }` (M57c) -- see OP_REC_OVERRIDES_MERGE's
 * own long comment, ir.h. `syms` is n entries in ORIGINAL (OP_FRAME_SET slot)
 * order, NOT sorted; `overrides_slot` is which of those n slots is
 * `__overrides` itself. Emitted by compiler.c's AST_ATTRSET case in place
 * of the usual `ib_frame_get(...)*n + ib_mk_attrs(...)` pair, only when a
 * rec literal has a binding literally named `__overrides`. */
size_t ib_rec_overrides_merge(IrBuilder *b, int64_t n, const uint32_t *syms, int64_t overrides_slot);

/* "operators" milestone: comparisons (INT-only for ordering, see
 * op_cmp.S), booleans, attrset update, assert. `&&`/`||` have no
 * dedicated op -- see ir.h's OP_EQ block comment. */
size_t ib_eq(IrBuilder *b);
size_t ib_neq(IrBuilder *b);
size_t ib_lt(IrBuilder *b);
size_t ib_gt(IrBuilder *b);
size_t ib_le(IrBuilder *b);
size_t ib_ge(IrBuilder *b);
size_t ib_not(IrBuilder *b);
size_t ib_update(IrBuilder *b);
size_t ib_concat(IrBuilder *b);
size_t ib_div(IrBuilder *b);
size_t ib_insert_dynamic_attr(IrBuilder *b);

/* `expr.${nameExpr} or default`'s try-site -- returns the emitted op's
 * index for the caller to patch with ib_patch_delta once the default
 * expression's end is known (see ib_select_or's identical pattern). */
size_t ib_select_dynamic_or(IrBuilder *b);
size_t ib_resolve_search_path(IrBuilder *b);
/* Emits OP_ASSERT_FAIL over a {msg, len} pool pair; `msg` must outlive the
 * resulting IrProgram (compiler.c arena-allocates it, which is never
 * freed, exactly like string literals -- see AST_STRING). */
size_t ib_assert_fail(IrBuilder *b, const char *msg, size_t len);

/* Pops `count` values (reverse push order) and pushes one concatenated
 * String -- see OP_STR_CONCAT in ir.h. */
size_t ib_str_concat(IrBuilder *b, int64_t count);

/* M59: same as ib_str_concat, but the result is re-canonicalized and
 * wrapped as a Path -- see OP_PATH_CONCAT in ir.h. Only ever emitted when
 * the first of the `count` pushed values is already a Path constant
 * (compiler.c's AST_PATH case). */
size_t ib_path_concat(IrBuilder *b, int64_t count);

/* `with`: pops the with-expr thunk (pushed by a preceding ib_mk_thunk, same
 * convention as ib_let) into a new with-tagged 1-slot Env frame -- see
 * ASNIX_ENV_WITH_TAG in env.h and op_with.S. */
size_t ib_with(IrBuilder *b);
/* Emits with a placeholder delta (operand `a`, patched via ib_patch_delta,
 * same convention as ib_if/ib_jmp) and the fixed symbol id `sym` (operand
 * `b`): pops the attrs value a preceding ib_var_ref forced/pushed,
 * searches for `sym`, and either falls through with the match on top of
 * VALSTACK or jumps by the (not-yet-patched) delta to try the next
 * enclosing `with`. */
size_t ib_with_try(IrBuilder *b, uint32_t sym);
/* Every enclosing `with` was tried and none had the symbol: aborts with
 * "undefined variable", same error class as the compile-time check. */
size_t ib_with_miss(IrBuilder *b);
/* M52: the with-frame counterpart of ib_frame_exit() -- restores ENV to the
 * with-frame's `up` (tag bit masked off) once the `with` body is fully
 * compiled, so a non-tail `with` doesn't leak its frame into a sibling's
 * compiled depths. See OP_WITH_EXIT in ir.h. */
size_t ib_with_exit(IrBuilder *b);

/* Lambda-patterns: pops a value, pushes TRUE/FALSE -- see op_has_attr.S. */
size_t ib_has_attr(IrBuilder *b, uint32_t sym);

/* `expr ? ${nameExpr}` (M33): pops the name Value then the target -- see
 * OP_HAS_ATTR_DYNAMIC (ir.h) and op_has_attr_dynamic.S. */
size_t ib_has_attr_dynamic(IrBuilder *b);

/* Lambda-patterns: eager pre-destructuring validation of the call argument
 * (see op_check_formals.S for the exact semantics and pool layout).
 * `required_syms_sorted` must have `required_count` entries, sorted
 * ascending. `all_syms_sorted` is read (and must have `total_count`
 * sorted-ascending entries) only when `total_count >= 0`; pass
 * `total_count = -1` and `all_syms_sorted = NULL` for a pattern with `...`
 * (skips the "no extra fields" check entirely). Per-slot binding of the
 * destructured fields reuses ib_frame_alloc()/ib_frame_set()/
 * ib_frame_exit() above -- recursive `let`'s own alloc-before-thunks
 * mechanism, not a second duplicate op pair. */
size_t ib_check_formals(IrBuilder *b, int64_t required_count, const uint32_t *required_syms_sorted,
                         int64_t total_count, const uint32_t *all_syms_sorted);

/* ll-only, no asnix equivalent: see ir.h's OP_MK_FORMALSMAP comment. */
size_t ib_mk_formalsmap(IrBuilder *b, int64_t nf, const int64_t *syms);

/* `expr.attr or default`: emits with a placeholder delta (operand `a`,
 * patched via ib_patch_delta, same convention as ib_if/ib_jmp/ib_with_try)
 * and the fixed symbol id `sym` (operand `b`) -- see OP_SELECT_OR in ir.h. */
size_t ib_select_or(IrBuilder *b, uint32_t sym);

/* M162: branch-fused comparison/`?`-membership ops -- see their OP_*_IF
 * comments in ir.h for the full rationale. Each emits with a placeholder
 * delta (operand `a`, patched via ib_patch_delta, same convention as
 * ib_if/ib_jmp), taken when the comparison/test is FALSE; a TRUE result
 * falls through to the very next op. Only compiler.c's AST_IF case emits
 * these (when the condition is literally a bare comparison/`?` test) --
 * never general-purpose replacements for ib_eq/ib_neq/ib_lt/ib_ge/
 * ib_has_attr, which remain unchanged for every other use (e.g. `&&`/
 * `||`'s own operands, or a comparison used as an ordinary non-`if`-
 * condition value). */
size_t ib_eq_if(IrBuilder *b);
size_t ib_neq_if(IrBuilder *b);
size_t ib_lt_if(IrBuilder *b);
size_t ib_ge_if(IrBuilder *b);
size_t ib_has_attr_if(IrBuilder *b, uint32_t sym);

/* Finalizes the builder: runs ir_link() over the accumulated ops in place
 * and returns an IrProgram ready for asnix_eval(). */
IrProgram *ib_finish(IrBuilder *b);

#endif /* ASNIX_TEST_IRBUILD_H */
