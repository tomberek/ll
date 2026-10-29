/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_VALUE_H
#define LL_VALUE_H

/*
 * 8-byte pointer-tagged word. See docs/value-encoding.md for full rationale.
 *
 * Bit layout:  [ ............. payload ............. ][ tag : 3 bits ]
 *                63                                   3 2 1 0
 *
 * All heap allocations are 8-byte aligned (enforced by the bump arena), so
 * PTR-tagged words are untagged pointers verbatim -- stripping the tag is a
 * no-op, and testing "is this tag T" is a register-only `and`+`cmp`, never a
 * memory load. Four tag slots (ATTRS/LIST/CLOSURE/THUNK) are spent on heap
 * *kinds* directly in the tag bits rather than a single generic PTR+header-
 * Kind-byte scheme, because the thunk-force check and call/select dispatch
 * are the hottest checks in the whole evaluator.
 *
 * This header is included by both C (src/c) and asm (src/asm, via cpp) so
 * the two sides can never drift on these constants. `cc` predefines
 * __ASSEMBLER__ while preprocessing a .S file, so every macro down to the
 * #ifndef __ASSEMBLER__ guard below is GAS-safe: plain integer literals
 * only, no C syntax, no literal suffixes (GAS's expression parser does not
 * understand C integer suffixes like ULL/LL). Everything inside that guard
 * is C-only (typedefs, prototypes, inline functions).
 */

#define LL_TAG_MASK   0x7
#define LL_TAG_BITS   3

#define LL_TAG_PTR      0x0 /* generic heap pointer (strings etc. via a header Kind byte) */
#define LL_TAG_INT      0x1 /* 61-bit immediate signed integer */
#define LL_TAG_IMM      0x2 /* small fixed enumeration (null/false/true/blackhole/...) */
#define LL_TAG_ATTRS    0x3 /* pointer to an attrset heap object */
#define LL_TAG_LIST     0x4 /* pointer to a list heap object */
#define LL_TAG_CLOSURE  0x5 /* pointer to a lambda/closure heap object */
#define LL_TAG_THUNK    0x6 /* pointer to a 2-word thunk cell */
#define LL_TAG_PRIMOP   0x7 /* pointer to a native builtin-function heap object (M7) */

/* IMM sub-tags, packed into bits [63:3] of an LL_TAG_IMM word. */
#define LL_IMM_NULL      0
#define LL_IMM_FALSE     1
#define LL_IMM_TRUE      2
#define LL_IMM_BLACKHOLE 3

/* 61-bit signed immediate integer range (M56): the ±2^60 window a
 * TAG_INT word can actually hold. Spelled as plain decimal literals (no
 * INT64_C/shift, no `LL` suffix) specifically so this is usable *both*
 * from C and as a GAS immediate operand after cpp substitution -- GAS's
 * own expression parser reads a bare decimal literal like
 * `1152921504606846975` directly, but chokes on C integer-suffix syntax.
 * Needed in asm now because op_push_int/op_arith.S's add/sub/mul/div
 * fast paths inline the overflow check directly (see those files) rather
 * than paying a `call` into ll_make_int per op -- same inlining
 * tradeoff value.h's own header comment already describes for the
 * tagging bit tricks themselves. */
#define LL_INT_MAX 1152921504606846975
#define LL_INT_MIN (-1152921504606846976)

#ifndef __ASSEMBLER__

#include <stdint.h>

typedef struct {
    uint64_t raw;
} Value;

/*
 * Implemented in src/asm/value.S, not as C inline functions: these are the
 * canonical, directly-unit-tested reference implementation of the tagging
 * scheme. The hot evaluator core (M3+) inlines the same bit tricks directly
 * in op handlers instead of paying a `call` per tag check -- these routines
 * exist for the C-side boundary (compiler, debug printing, tests).
 */
int ll_tag_of(Value v);
Value ll_make_int(int64_t n);
int64_t ll_int_value(Value v);
Value ll_make_imm(int subtag);
int ll_imm_subtag(Value v);
int ll_is_thunk(Value v);

static inline int ll_values_equal(Value a, Value b) {
    return a.raw == b.raw;
}

#define LL_NULL      (ll_make_imm(LL_IMM_NULL))
#define LL_TRUE      (ll_make_imm(LL_IMM_TRUE))
#define LL_FALSE     (ll_make_imm(LL_IMM_FALSE))
#define LL_BLACKHOLE (ll_make_imm(LL_IMM_BLACKHOLE))

#endif /* __ASSEMBLER__ */

#endif /* LL_VALUE_H */
