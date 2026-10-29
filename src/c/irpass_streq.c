/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "ll/irpass.h"

#include <stdint.h>
#include <string.h>

#include "value_util.h"

static int op_is_jump(int op_id) {
    return op_id == OP_IF || op_id == OP_JMP || op_id == OP_WITH_TRY || op_id == OP_SELECT_OR ||
           op_id == OP_SELECT_DYNAMIC_OR || op_id == OP_EQ_IF || op_id == OP_NEQ_IF || op_id == OP_LT_IF ||
           op_id == OP_GE_IF || op_id == OP_HAS_ATTR_IF;
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
        int64_t target = (int64_t)i + ops[i].a / LL_IROP_SIZE;
        if (target >= (int64_t)start && target < (int64_t)(start + count)) return 1;
    }
    return 0;
}

static int is_genuine_string(Value v) {
    if (ll_tag_of(v) != LL_TAG_PTR) return 0;
    if (ll_value_is_path(v)) return 0;
    if (ll_value_is_float(v)) return 0;
    if (ll_value_is_cell(v)) return 0;
    if (ll_value_is_lazyapp(v)) return 0;
    return 1;
}

static int fold_const_string_eq(IrOp *ops, size_t *len, int64_t **pool, size_t *pool_len, size_t *pool_cap) {
    (void)pool_cap;
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
        const char *b1 = ll_ptr_bytes(v1, &len1);
        const char *b2 = ll_ptr_bytes(v2, &len2);
        int equal = (len1 == len2) && (len1 == 0 || memcmp(b1, b2, (size_t)len1) == 0);
        int result = (op_id == OP_EQ) ? equal : !equal;

        ops[i].handler = (void *)(intptr_t)(result ? OP_PUSH_TRUE : OP_PUSH_FALSE);
        ops[i].a = 0;
        ops[i].b = 0;
        ll_irpass_remove(ops, len, i + 1, 2);
        return 1;
    }
    return 0;
}

void ll_irpass_register_streq(void) {
    ll_irpass_register("streq", fold_const_string_eq);
}
