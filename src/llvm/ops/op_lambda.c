/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445's src/asm/ops/op_lambda.S and op_mk_thunk.S -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "ops.h"

LL_OP_ATTR Value ll_op_lambda(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    int64_t arity = ip->a;
    int64_t pidx = ip->b;
    IrProgram *body_prog = (IrProgram *)(uintptr_t)pool[pidx];
    Value formals;
    formals.raw = (uint64_t)pool[pidx + 1];

    Closure *c = (Closure *)ll_arena_alloc_fast(LL_CLOSURE_SIZE);
    c->up = env;
    c->prog = body_prog;
    c->arity = arity;
    c->formals = formals;

    ll_check_valstack(vs);
    vs[0].raw = ((uint64_t)(uintptr_t)c) | LL_TAG_CLOSURE;
    vs++;
    LL_NEXT(ip, env, vs, pool, rs);
}

LL_OP_ATTR Value ll_op_mk_thunk(IrOp *ip, Env *env, Value *vs, int64_t *pool, RetFrame *rs) {
    int64_t pidx = ip->b;
    IrProgram *body_prog = (IrProgram *)(uintptr_t)pool[pidx];

    Thunk *t = (Thunk *)ll_arena_alloc_fast(LL_THUNK_SIZE);
    t->state = (int64_t)(uintptr_t)body_prog; /* 8-byte-aligned pointer -- tag bits 0 == UNFORCED */
    t->env = (uint64_t)(uintptr_t)env;

    ll_check_valstack(vs);
    vs[0].raw = ((uint64_t)(uintptr_t)t) | LL_TAG_THUNK;
    vs++;
    LL_NEXT(ip, env, vs, pool, rs);
}
