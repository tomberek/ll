/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_SYMTAB_H
#define LL_SYMTAB_H

#include <stddef.h>
#include <stdint.h>

/*
 * Dense u32 symbol interning. Hashing is a cheap multiply-based mix (FxHash
 * shape) finished with a splitmix64-style avalanche, not SipHash -- these
 * are compiler-internal identifiers (attr names, let/lambda bindings),
 * never adversarial input, and `~/gnix/iets` specifically measured SipHash
 * as a profiler-visible cost for exactly this kind of short, high-volume
 * string hashing (see docs/architecture.md's prior-art notes).
 */

uint32_t ll_intern(const char *name, size_t len);

/* Reverse lookup, mainly for error messages/debugging. */
const char *ll_sym_name(uint32_t id, size_t *out_len);

#endif /* LL_SYMTAB_H */
