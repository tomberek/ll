/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "asnix/irpass.h"

#include <stdint.h>

/* Dead zero-slot frame elimination -- a backstop catching a
 * `FRAME_ALLOC a=0 ... FRAME_EXIT` bracket (the body between them stays,
 * it still needs to run for its own side effects/value-production -- only
 * the frame bracket itself is dead weight) that genuinely has no
 * surviving reference to its own (zero-slot, therefore necessarily empty)
 * frame anywhere in its own body.
 *
 * This is NOT trying to re-do M114 (compiler.c's AST_ATTRSET case
 * skipping ib_frame_alloc entirely for a non-rec literal with
 * hidden_count==0) -- that case never emits FRAME_ALLOC/FRAME_EXIT at
 * all, so there is nothing here for this pass to find. What DOES still
 * emit an a=0 FRAME_ALLOC/FRAME_EXIT pair straight from source,
 * unconditionally, is AST_LET (compiler.c): `ib_frame_alloc(b,
 * (int64_t)n)` runs with `n == node->as.let1.count` whatever that count
 * is, including 0 for a bindings-free `let in body` -- confirmed directly
 * (`./irdump` on `let in 1` prints exactly FRAME_ALLOC a=0 / PUSH_INT /
 * FRAME_EXIT / HALT, with nothing between the two frame ops touching the
 * frame at all). A plausible SECOND source, not yet observed from any
 * compiled fixture but structurally identical in shape, is some future
 * pass that empties out an originally nonzero FRAME_SET/FRAME_GET body
 * without itself renumbering FRAME_ALLOC's own `a` -- this pass only
 * acts on whatever `a` already reads at the time it runs, it does not
 * itself ever produce a zero from a nonzero `a`.
 *
 * THE DEPTH HAZARD (the one real subtlety here, confirmed empirically,
 * not just suspected): FRAME_ALLOC installs a brand-new Env and FRAME_
 * EXIT undoes exactly that one push (op_frame.S) -- removing the pair
 * changes how many real hops OP_VAR_REF/OP_VAR_REF_CELL/OP_VAR_REF_RAW's
 * own `depth` field (op_var_ref.S: walks ENV->up exactly `depth` times)
 * has to walk for anything INSIDE the bracket that reaches past this
 * frame to an enclosing one. This is reachable from completely ordinary
 * source, not a hypothetical: `let a = 1; in (let in a)` compiles (ir
 * dump, confirmed directly) to
 *   FRAME_ALLOC a=1 ; PUSH_INT 1 ; FRAME_SET a=0 ;
 *   FRAME_ALLOC a=0 ; VAR_REF depth=1 slot=0 ; FRAME_EXIT ; FRAME_EXIT
 * -- the inner (zero-slot, dead-bracket candidate) frame's own body is a
 * single VAR_REF with depth=1 (not 0!), reaching past the inner frame to
 * the outer one. Blindly deleting the inner FRAME_ALLOC/FRAME_EXIT pair
 * without also decrementing that VAR_REF's depth by one would silently
 * make it walk one hop too far at runtime -- a real miscompile, not a
 * missed optimization. So "no FRAME_SET/FRAME_GET and no OP_VAR_REF*
 * depth==0" (this frame's OWN depth, always vacuously true anyway since
 * a zero-slot frame never has a valid slot to address) is NOT by itself
 * a sufficient safety condition; this pass additionally tracks how many
 * FURTHER frame/with pushes have opened (and not yet closed) since its
 * own candidate FRAME_ALLOC at each point in the scan (`local_nest`
 * below), so a VAR_REF-family op's depth can be judged RELATIVE TO THIS
 * CANDIDATE FRAME specifically (its own hop is exactly `local_nest` at
 * that point) rather than relative to whatever frame is innermost there:
 *   - depth < local_nest: reaches only as far as some frame nested
 *     INSIDE the candidate bracket -- untouched by removing the
 *     candidate, no adjustment needed.
 *   - depth == local_nest: reaches EXACTLY the candidate's own (zero-
 *     slot, so never actually a valid target) frame -- disqualifying,
 *     checked defensively even though it should never actually occur.
 *   - depth > local_nest: reaches PAST the candidate frame to something
 *     enclosing it -- safe to remove the candidate, but only once this
 *     op's own depth is decremented by one to compensate for the removed
 *     hop.
 * OP_FRAME_SET/OP_FRAME_GET/OP_CHECK_FORMALS/OP_REC_OVERRIDES_MERGE all
 * address the CURRENT frame's own slots directly (no depth field at
 * all -- ir.h/op_frame.S) -- equivalent to "depth 0 relative to whatever
 * frame is innermost right now", so these only disqualify when
 * `local_nest == 0` (meaning "innermost right now" IS the candidate
 * frame itself); under any nested frame (`local_nest > 0`) they operate
 * on that nested frame instead, untouched by removing the outer
 * candidate.
 *
 * Nesting note: FRAME_ALLOC/FRAME_EXIT pairs are always strictly nested
 * and balanced in this compiler's own output (confirmed directly: `let
 * in (let in 1)` IR-dumps as FRAME_ALLOC;FRAME_ALLOC;PUSH_INT;FRAME_EXIT;
 * FRAME_EXIT;HALT, properly nested) -- so a simple depth counter finds
 * each candidate's own matching FRAME_EXIT unambiguously. OP_WITH/OP_
 * WITH_EXIT are balanced the same way (compiler.c's AST_WITH: ib_with_
 * exit always undoes exactly the ib_with just above it) and ALSO
 * consume one real ENV hop (masked for the with-tag, not skipped --
 * op_var_ref.S's own walk), so they're tracked by the same `local_nest`
 * counter as FRAME_ALLOC/FRAME_EXIT.
 *
 * Standard incoming-jump safety check (same pattern as irpass_jmpnop.c's
 * own is_targeted_by_another_jump): a FRAME_ALLOC or FRAME_EXIT op is
 * not itself jump-bearing, but CAN be a live jump's TARGET -- e.g. `let
 * in (if c then 1 else 2)` lands the inner if's own join-point JMP
 * target exactly on the outer FRAME_EXIT (the first op after the whole
 * if-expression). asnix_irpass_remove's own safety check would already
 * catch this and abort the whole process (fatal), so -- same discipline
 * as every other pass in this framework -- check first and simply
 * decline (continue the outer scan) rather than ever reaching that. */

static int op_is_jump(int op_id) {
    return op_id == OP_IF || op_id == OP_JMP || op_id == OP_WITH_TRY || op_id == OP_SELECT_OR ||
           op_id == OP_SELECT_DYNAMIC_OR || op_id == OP_EQ_IF || op_id == OP_NEQ_IF || op_id == OP_LT_IF ||
           op_id == OP_GE_IF || op_id == OP_HAS_ATTR_IF; /* M162 */
}

static int is_targeted_by_another_jump(IrOp *ops, size_t len, size_t idx) {
    for (size_t j = 0; j < len; j++) {
        if (j == idx) continue;
        int id = (int)(intptr_t)ops[j].handler;
        if (!op_is_jump(id)) continue;
        int64_t target = (int64_t)j + ops[j].a / (int64_t)ASNIX_IROP_SIZE;
        if (target == (int64_t)idx) return 1;
    }
    return 0;
}

static int op_is_var_ref_family(int id) {
    return id == OP_VAR_REF || id == OP_VAR_REF_CELL || id == OP_VAR_REF_RAW;
}

/* Current-frame-slot-addressing ops -- no depth field, always implicitly
 * "whatever frame is innermost right now" (see file header comment). */
static int op_touches_innermost_frame_slots(int id) {
    return id == OP_FRAME_SET || id == OP_FRAME_GET || id == OP_CHECK_FORMALS || id == OP_REC_OVERRIDES_MERGE;
}

/* Finds the FRAME_EXIT matching the FRAME_ALLOC at `alloc_idx` (same
 * nesting level), or (size_t)-1 if the array is malformed (never actually
 * happens from this compiler's own output -- every FRAME_ALLOC this
 * project ever emits is paired; checked defensively anyway). */
static size_t find_matching_frame_exit(IrOp *ops, size_t len, size_t alloc_idx) {
    int depth = 1;
    for (size_t i = alloc_idx + 1; i < len; i++) {
        int id = (int)(intptr_t)ops[i].handler;
        if (id == OP_FRAME_ALLOC) {
            depth++;
        } else if (id == OP_FRAME_EXIT) {
            depth--;
            if (depth == 0) return i;
        }
    }
    return (size_t)-1;
}

/* Scans strictly between a candidate FRAME_ALLOC (index `i`) and its
 * matching FRAME_EXIT (index `j`), tracking `local_nest` (see file header
 * comment) to decide whether every ENV-touching op in that range is safe
 * w.r.t. removing the candidate bracket. Returns 1 if safe. */
static int range_is_safe_to_elide(IrOp *ops, size_t i, size_t j) {
    int local_nest = 0;
    for (size_t k = i + 1; k < j; k++) {
        int id = (int)(intptr_t)ops[k].handler;
        if (id == OP_FRAME_ALLOC || id == OP_WITH) {
            local_nest++;
            continue;
        }
        if (id == OP_FRAME_EXIT || id == OP_WITH_EXIT) {
            local_nest--;
            continue;
        }
        if (op_touches_innermost_frame_slots(id)) {
            if (local_nest == 0) return 0; /* addresses OUR candidate frame's (nonexistent) slots */
            continue;
        }
        if (op_is_var_ref_family(id)) {
            if (ops[k].a == (int64_t)local_nest) return 0; /* reaches exactly our candidate frame */
            continue; /* < local_nest: nested-only, untouched; > local_nest: crosses us, fixed up below */
        }
    }
    return 1;
}

/* Second pass, only run once range_is_safe_to_elide has already confirmed
 * this candidate commits: decrements by one the depth of every VAR_REF-
 * family op that crosses the candidate frame (depth > local_nest at its
 * own point), compensating for the one real ENV hop about to disappear. */
static void apply_depth_fixup(IrOp *ops, size_t i, size_t j) {
    int local_nest = 0;
    for (size_t k = i + 1; k < j; k++) {
        int id = (int)(intptr_t)ops[k].handler;
        if (id == OP_FRAME_ALLOC || id == OP_WITH) {
            local_nest++;
            continue;
        }
        if (id == OP_FRAME_EXIT || id == OP_WITH_EXIT) {
            local_nest--;
            continue;
        }
        if (op_is_var_ref_family(id) && ops[k].a > (int64_t)local_nest) {
            ops[k].a -= 1;
        }
    }
}

static int eliminate_dead_zero_frame(IrOp *ops, size_t *len, int64_t **pool, size_t *pool_len, size_t *pool_cap) {
    (void)pool;
    (void)pool_len;
    (void)pool_cap; /* this pass only removes a dead frame bracket and fixes up depth fields,
                      * never touches the constant pool. */
    size_t n = *len;
    for (size_t i = 0; i + 1 < n; i++) {
        if ((int)(intptr_t)ops[i].handler != OP_FRAME_ALLOC) continue;
        if (ops[i].a != 0) continue; /* only a genuinely zero-slot frame is a candidate */

        size_t j = find_matching_frame_exit(ops, n, i);
        if (j == (size_t)-1) continue;
        if (!range_is_safe_to_elide(ops, i, j)) continue;
        if (is_targeted_by_another_jump(ops, n, i)) continue;
        if (is_targeted_by_another_jump(ops, n, j)) continue;

        apply_depth_fixup(ops, i, j);
        /* Remove FRAME_EXIT (the higher index) first so FRAME_ALLOC's own
         * index `i` isn't shifted out from under the second removal. */
        asnix_irpass_remove(ops, len, j, 1);
        asnix_irpass_remove(ops, len, i, 1);
        return 1; /* re-scan from the top next iteration -- simplest correct strategy */
    }
    return 0;
}

void asnix_irpass_register_deadframe(void) {
    asnix_irpass_register("deadframe", eliminate_dead_zero_frame);
}
