/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "ll/irpass.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "value_util.h"

/* Folds `OP_PUSH_CONST idx1; OP_PUSH_CONST idx2; OP_STR_CONCAT a=2` into a
 * single `OP_PUSH_CONST idx3` whenever BOTH pool entries are already
 * genuine String values -- the bytecode-level mirror of what
 * compiler.c's own AST_STRING case already does at compile time for an
 * ENTIRELY-literal string ("no interpolation at all" -> one constant),
 * except this also catches the two-constant case exposed only once two
 * adjacent parts of an interpolated string EACH happen to compile down to
 * a bare constant on their own -- e.g. `"${"a"}${"b"}"` (two nested,
 * individually-all-literal interpolations, each folded by compiler.c into
 * its own OP_PUSH_CONST, landing adjacent to each other before a single
 * `a=2` OP_STR_CONCAT) -- or, same spirit as irpass_cfold.c's own
 * comment, a pair exposed later by some OTHER pass's prior rewrite, not
 * just something already foldable purely from the original source.
 *
 * Deliberately scoped to the SAFE, simple a==2 case only (not any N>2
 * OP_STR_CONCAT, and not a mix of one-constant-one-not): an
 * OP_PUSH_CONST's pool entry can hold ANY constant Value shape (an int,
 * bool, list-of-syms, a Path or Float that merely happens to share
 * LL_TAG_PTR with String, ...) -- there's no way to tell from the op
 * alone, so this pass must actually dereference the pool entry and check
 * its runtime tag before assuming "string". A Path or Float operand is
 * deliberately left alone (not folded) even though both ALSO reach
 * OP_STR_CONCAT via the exact same op shape: interpolating either one has
 * real side effects at runtime (Path: existence check + NAR-copy-to-store
 * coercion, value_util.c's coerce_to_string_for_interp; Float: rejected
 * outright, "cannot coerce a float to a string") that a compile-time fold
 * must not silently skip.
 *
 * String "context" (M76, string_context.h) is a non-issue here: it's
 * attached via a side table keyed by a String's own arena pointer, and
 * ONLY ever populated by RUNTIME construction call sites (derivation
 * outputs, path/attrs coercion, ...) -- a literal pool constant built by
 * compiler.c (either directly, or via ll_make_string_value in its own
 * AST_STRING interpolation case) never goes through
 * ll_string_attach_context, so both operands here are guaranteed
 * context-free already; the freshly-built folded constant below
 * inherits that same "no context" state for free, identical to what
 * OP_STR_CONCAT's own runtime path (concat_strings_impl, unioning two
 * empty contexts) would have produced. */
/* Which op-ids carry a byte-delta-to-target in field `a` -- see ir.h's own
 * per-op comments; same exhaustive list irpass.c's own op_is_jump uses. */
static int op_is_jump(int op_id) {
    return op_id == OP_IF || op_id == OP_JMP || op_id == OP_WITH_TRY || op_id == OP_SELECT_OR ||
           op_id == OP_SELECT_DYNAMIC_OR || op_id == OP_EQ_IF || op_id == OP_NEQ_IF || op_id == OP_LT_IF ||
           op_id == OP_GE_IF || op_id == OP_HAS_ATTR_IF; /* M162 */
}

/* Found on a REAL nixpkgs workload (not caught by the 308-fixture +
 * 806-file nix-conformance sweep this pass was originally verified
 * against): the second OP_PUSH_CONST (ops[i+1]) is not always reached by
 * falling through from the first (ops[i]) -- `"${expr.attr or "default"}
 * literal"` compiles `expr.attr or "default"` via OP_SELECT_OR, whose
 * FOUND path pushes the found value and falls through to an OP_JMP that
 * skips straight to this interpolation's next literal chunk (exactly
 * ops[i+1]'s position), bypassing ops[i] (the MISS path's own default-
 * value constant) entirely. Folding ops[i]+ops[i+1] into one constant
 * assumes ops[i] ALWAYS executes first, which is false on that FOUND
 * path. ll_irpass_remove's own safety check caught this (a live jump
 * targets the removed range) rather than letting it corrupt the FOUND
 * case's result. Fix: only fold when nothing jumps directly into either
 * op about to be removed -- same incoming-jump guard irpass_dce_if.c's
 * own range_has_incoming_jump establishes for exactly this hazard class
 * (duplicated here, not shared, to keep this pass self-contained in its
 * own file per this framework's convention). */
static int range_has_incoming_jump(IrOp *ops, size_t len, size_t start, size_t count) {
    for (size_t i = 0; i < len; i++) {
        int id = (int)(intptr_t)ops[i].handler;
        if (!op_is_jump(id)) continue;
        int64_t target = (int64_t)i + ops[i].a / LL_IROP_SIZE;
        if (target >= (int64_t)start && target < (int64_t)(start + count)) return 1;
    }
    return 0;
}

static int fold_adjacent_const_strings(IrOp *ops, size_t *len, int64_t **pool, size_t *pool_len, size_t *pool_cap) {
    for (size_t i = 0; i + 2 < *len; i++) {
        if ((int)(intptr_t)ops[i].handler != OP_PUSH_CONST) continue;
        if ((int)(intptr_t)ops[i + 1].handler != OP_PUSH_CONST) continue;
        if ((int)(intptr_t)ops[i + 2].handler != OP_STR_CONCAT) continue;
        if (ops[i + 2].a != 2) continue;
        if (range_has_incoming_jump(ops, *len, i + 1, 2)) continue; /* ops[i+1]/ops[i+2] reachable
                                                                       * some OTHER way -- don't fold */

        int64_t idx1 = ops[i].b, idx2 = ops[i + 1].b;
        if (idx1 < 0 || (size_t)idx1 >= *pool_len) continue;
        if (idx2 < 0 || (size_t)idx2 >= *pool_len) continue;

        Value v1 = { .raw = (uint64_t)(*pool)[idx1] };
        Value v2 = { .raw = (uint64_t)(*pool)[idx2] };
        /* "genuine String" check -- same tag-exclusion pattern
         * value_util.c's own is_plain_string helper uses (LL_TAG_PTR
         * minus the Path/Float sibling kinds that share it); Cell/
         * LazyApp are excluded too purely for defense-in-depth (neither
         * can actually appear in a compile-time constant pool -- both
         * are purely-internal runtime-only devices, see heap.h -- but
         * checking costs nothing and keeps this pass correct even if
         * that ever changed). */
        if (ll_tag_of(v1) != LL_TAG_PTR || ll_value_is_path(v1) || ll_value_is_float(v1) ||
            ll_value_is_cell(v1) || ll_value_is_lazyapp(v1))
            continue;
        if (ll_tag_of(v2) != LL_TAG_PTR || ll_value_is_path(v2) || ll_value_is_float(v2) ||
            ll_value_is_cell(v2) || ll_value_is_lazyapp(v2))
            continue;

        int64_t len1, len2;
        const char *b1 = ll_ptr_bytes(v1, &len1);
        const char *b2 = ll_ptr_bytes(v2, &len2);

        /* OP_STR_CONCAT's `parts` array (op_str_concat.S) is VALSTACK's
         * last `count` slots read base-to-top, i.e. in ORIGINAL PUSH
         * order (ops[i]'s value, pushed first, is parts[0]) -- NOT
         * reversed; concat_strings_impl (value_util.c) then concatenates
         * parts[0..count) in that same order. So the fold below must
         * place v1's bytes (ops[i], pushed/popped-last-conceptually but
         * array-index-0) before v2's (ops[i+1]) -- exactly source order,
         * no reversal needed. */
        char *buf = malloc((size_t)(len1 + len2));
        if (buf == NULL) continue; /* allocation failure: leave this triple for the runtime op */
        memcpy(buf, b1, (size_t)len1);
        memcpy(buf + len1, b2, (size_t)len2);
        Value folded = ll_make_string_value(buf, (size_t)(len1 + len2));
        free(buf);

        size_t new_idx = ll_irpass_pool_append(pool, pool_len, pool_cap, (int64_t)folded.raw);
        ops[i].handler = (void *)(intptr_t)OP_PUSH_CONST;
        ops[i].a = 0;
        ops[i].b = (int64_t)new_idx;
        ll_irpass_remove(ops, len, i + 1, 2);
        return 1; /* re-scan from the top next iteration -- simplest correct strategy */
    }
    return 0;
}

void ll_irpass_register_strconcat(void) {
    ll_irpass_register("strconcat", fold_adjacent_const_strings);
}
