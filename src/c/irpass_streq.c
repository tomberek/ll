/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "asnix/irpass.h"

#include <stdint.h>
#include <string.h>

#include "value_util.h"

/* Folds `OP_PUSH_CONST idx1; OP_PUSH_CONST idx2; OP_EQ`/`OP_NEQ` into a
 * single `OP_PUSH_TRUE`/`OP_PUSH_FALSE` whenever BOTH pool entries are
 * already genuine String values -- the EQ/NEQ-flavored sibling of
 * irpass_strconcat.c's own adjacent-constant fold, extending irpass_cfold.
 * c's established "PUSH;PUSH;binop -> single push" pattern (previously
 * int-only) to this one additional, provably-safe case.
 *
 * Byte-for-byte string comparison (length, then memcmp) is always a cheap,
 * pure, side-effect-free operation with no coercion ambiguity, UNLIKE
 * cfold's own int/float-adjacent concerns (OP_DIV's zero-divisor/overflow
 * exclusions) or strconcat's own Path/Float exclusions (NAR-copy coercion;
 * "cannot coerce a float to a string" fatal) -- confirmed directly against
 * this project's own String representation (include/asnix/heap.h's
 * ASNIX_STRING_OFF_LEN/OFF_BYTES: a fixed-length byte buffer, no NUL
 * terminator reliance, no encoding games) and against op_cmp.S/
 * builtins_shared.c's own asnix_bshared_value_eq (the function OP_EQ/
 * OP_NEQ actually call, via asnix_bshared_value_eq_op): for a ta==tb==
 * ASNIX_TAG_PTR pair it does exactly `la == lb && (la == 0 || memcmp(ba,
 * bb, la) == 0)` -- length-then-memcmp, nothing else, no context
 * involvement of any kind. That's also WHY this fold needs no context
 * handling at all (unlike strconcat's own union-of-contexts result):
 * real Nix string EQUALITY (unlike concatenation) never consults string
 * context -- confirmed both by asnix_bshared_value_eq's own code above
 * (there is no context lookup anywhere in its ASNIX_TAG_PTR branch) and by
 * upstream Nix's own eval.cc EvalState::eqValues (string case is a plain
 * `s1 == s2` on the underlying std::string_view, context is carried
 * entirely separately and never consulted by `==`). And even if EQ/NEQ DID
 * care about context, it would still be moot here specifically: a literal
 * OP_PUSH_CONST pool entry is built purely at compile time (compiler.c's
 * AST_STRING case / asnix_make_string_value) and is NEVER routed through
 * asnix_string_attach_context (a side table only ever populated by RUNTIME
 * construction call sites -- derivation outputs, path/attrs coercion --
 * see string_context.h and irpass_strconcat.c's own identical observation)
 * -- so both operands here are guaranteed context-free already, same as
 * strconcat's two operands are.
 *
 * Deliberately scoped to the SAFE, simple two-PUSH_CONST case only, same
 * discipline as irpass_strconcat.c: an OP_PUSH_CONST's pool entry can hold
 * ANY constant Value shape (int, bool, list-of-syms, a Path or Float that
 * merely happens to share ASNIX_TAG_PTR with String, ...) -- there's no
 * way to tell from the op alone, so this pass must actually dereference
 * the pool entry and check its runtime tag before assuming "string". A
 * Path operand is deliberately left alone (not folded) even though two
 * Paths would ALSO byte-compare correctly here: reusing strconcat's exact
 * tag-exclusion check (rather than re-deriving a possibly-incomplete one)
 * is the documented discipline for this class of pass, and Path equality
 * is out of this pass's stated scope (string EQ/NEQ only). */
static int op_is_jump(int op_id) {
    return op_id == OP_IF || op_id == OP_JMP || op_id == OP_WITH_TRY || op_id == OP_SELECT_OR ||
           op_id == OP_SELECT_DYNAMIC_OR || op_id == OP_EQ_IF || op_id == OP_NEQ_IF || op_id == OP_LT_IF ||
           op_id == OP_GE_IF || op_id == OP_HAS_ATTR_IF; /* M162 */
}

/* Same incoming-jump guard irpass_dce_if.c's own range_has_incoming_jump /
 * irpass_strconcat.c's own copy establish for exactly this hazard class
 * (duplicated here, not shared, to keep this pass self-contained in its
 * own file per this framework's convention) -- e.g. `"${expr.attr or
 * "default"}" == "x"`-shaped code where ops[i+1] (the second PUSH_CONST)
 * is reached via a jump that skips straight past ops[i], not by falling
 * through from it; folding would assume ops[i] always runs first, which
 * is false on that path. */
static int range_has_incoming_jump(IrOp *ops, size_t len, size_t start, size_t count) {
    for (size_t i = 0; i < len; i++) {
        int id = (int)(intptr_t)ops[i].handler;
        if (!op_is_jump(id)) continue;
        int64_t target = (int64_t)i + ops[i].a / ASNIX_IROP_SIZE;
        if (target >= (int64_t)start && target < (int64_t)(start + count)) return 1;
    }
    return 0;
}

/* "genuine String" check -- the EXACT tag-exclusion pattern
 * irpass_strconcat.c's own fold_adjacent_const_strings uses (ASNIX_TAG_PTR
 * minus the Path/Float/Cell/LazyApp sibling kinds that share it; Cell/
 * LazyApp excluded purely for defense-in-depth, same reasoning as there --
 * neither can actually appear in a compile-time constant pool, but
 * checking costs nothing). Copied rather than re-derived, per this pass's
 * own brief: strconcat.c's comment already explains why each exclusion
 * matters. */
static int is_genuine_string(Value v) {
    if (asnix_tag_of(v) != ASNIX_TAG_PTR) return 0;
    if (asnix_value_is_path(v)) return 0;
    if (asnix_value_is_float(v)) return 0;
    if (asnix_value_is_cell(v)) return 0;
    if (asnix_value_is_lazyapp(v)) return 0;
    return 1;
}

static int fold_const_string_eq(IrOp *ops, size_t *len, int64_t **pool, size_t *pool_len, size_t *pool_cap) {
    (void)pool_cap; /* this pass only READS the pool (never appends -- the folded result is
                      * OP_PUSH_TRUE/OP_PUSH_FALSE, not a new constant), unlike strconcat's own
                      * use of asnix_irpass_pool_append; still takes the full signature every
                      * pass does (see irpass.h). */
    for (size_t i = 0; i + 2 < *len; i++) {
        if ((int)(intptr_t)ops[i].handler != OP_PUSH_CONST) continue;
        if ((int)(intptr_t)ops[i + 1].handler != OP_PUSH_CONST) continue;
        int op_id = (int)(intptr_t)ops[i + 2].handler;
        if (op_id != OP_EQ && op_id != OP_NEQ) continue;
        if (range_has_incoming_jump(ops, *len, i + 1, 2)) continue; /* ops[i+1]/ops[i+2] reachable
                                                                       * some OTHER way -- don't fold */

        int64_t idx1 = ops[i].b, idx2 = ops[i + 1].b;
        if (idx1 < 0 || (size_t)idx1 >= *pool_len) continue;
        if (idx2 < 0 || (size_t)idx2 >= *pool_len) continue;

        Value v1 = { .raw = (uint64_t)(*pool)[idx1] };
        Value v2 = { .raw = (uint64_t)(*pool)[idx2] };
        if (!is_genuine_string(v1) || !is_genuine_string(v2)) continue;

        int64_t len1, len2;
        const char *b1 = asnix_ptr_bytes(v1, &len1);
        const char *b2 = asnix_ptr_bytes(v2, &len2);
        int equal = (len1 == len2) && (len1 == 0 || memcmp(b1, b2, (size_t)len1) == 0);
        int result = (op_id == OP_EQ) ? equal : !equal;

        ops[i].handler = (void *)(intptr_t)(result ? OP_PUSH_TRUE : OP_PUSH_FALSE);
        ops[i].a = 0;
        ops[i].b = 0;
        asnix_irpass_remove(ops, len, i + 1, 2);
        return 1; /* re-scan from the top next iteration -- simplest correct strategy */
    }
    return 0;
}

void asnix_irpass_register_streq(void) {
    asnix_irpass_register("streq", fold_const_string_eq);
}
