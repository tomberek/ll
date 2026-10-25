/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445's src/asm/ops/op_push.S -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "ops.h"

Value ll_op_push_int(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    int64_t raw = pool[ip->b];
    if (raw > ASNIX_INT_MAX || raw < ASNIX_INT_MIN) {
        static const char msg[] = "asnix: integer literal out of asnix's representable range, aborting\n";
        asnix_fatal(msg, sizeof(msg) - 1);
    }
    ll_check_valstack(vs);
    vs[0].raw = ((uint64_t)raw << ASNIX_TAG_BITS) | ASNIX_TAG_INT;
    vs++;
    LL_NEXT(ip, env, vs, pool, rs);
}

Value ll_op_push_true(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    ll_check_valstack(vs);
    vs[0] = ll_true();
    vs++;
    LL_NEXT(ip, env, vs, pool, rs);
}

Value ll_op_push_false(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    ll_check_valstack(vs);
    vs[0] = ll_false();
    vs++;
    LL_NEXT(ip, env, vs, pool, rs);
}

Value ll_op_push_null(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    ll_check_valstack(vs);
    vs[0].raw = ((uint64_t)ASNIX_IMM_NULL << ASNIX_TAG_BITS) | ASNIX_TAG_IMM;
    vs++;
    LL_NEXT(ip, env, vs, pool, rs);
}

Value ll_op_push_const(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    ll_check_valstack(vs);
    vs[0].raw = (uint64_t)pool[ip->b];
    vs++;
    LL_NEXT(ip, env, vs, pool, rs);
}

/* M10 perf: a fused run of `ip->a` consecutive OP_PUSH_INT ops with
 * contiguous pool indices starting at `ip->b`, assembled by
 * dispatch_table.c's post-link peephole pass (not emitted by the
 * compiler -- see that file's own comment). Replaces what would
 * otherwise be `ip->a` separate musttail hops with one hop and a
 * tight loop. A literal list/attrset built from plain integer
 * constants (e.g. bench/workloads/list_build_large.nix) compiles to
 * exactly this shape: a long run of bare OP_PUSH_INT, nothing
 * interleaved, immediately followed by OP_MK_LIST/OP_MK_ATTRS. */
Value ll_op_push_int_run(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    int64_t count = ip->a;
    int64_t start = ip->b;
    if ((void *)(vs + count) > asnix_valstack_limit) asnix_valstack_overflow();
    for (int64_t k = 0; k < count; k++) {
        int64_t raw = pool[start + k];
        if (raw > ASNIX_INT_MAX || raw < ASNIX_INT_MIN) {
            static const char msg[] = "asnix: integer literal out of asnix's representable range, aborting\n";
            asnix_fatal(msg, sizeof(msg) - 1);
        }
        vs[k].raw = ((uint64_t)raw << ASNIX_TAG_BITS) | ASNIX_TAG_INT;
    }
    vs += count;
    ip += count;
    LL_GOTO(ip, env, vs, pool, rs);
}
