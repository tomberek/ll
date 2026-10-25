/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef ASNIX_BUILTINS_TRYEVAL_H
#define ASNIX_BUILTINS_TRYEVAL_H

#include <stddef.h>

#include "asnix/value.h"
#include "builtins_misc.h" /* shared AsnixBuiltinMiscDef/-Builder {name, build()} shape */

/*
 * `builtins.tryEval` -- M30. Own translation unit: the setjmp/longjmp
 * catch mechanism it needs (tryeval.c/.h) and its special "do not pre-
 * force my argument" PrimOp (asnix_bshared_make_primop_noforce) are both
 * narrow enough, and specific enough to this one builtin, that folding
 * this into an existing module would obscure rather than share anything.
 * Registered under `builtins.tryEval` only -- real Nix's own registered
 * name is `__tryEval` (confirmed in cppnix's primops.cc), so unlike
 * `fromTOML`/`removeAttrs`/etc. this is NOT one of the bare-global names
 * (see builtins.h's asnix_make_import_primop comment for the full list).
 */

extern const AsnixBuiltinMiscDef ASNIX_TRYEVAL_BUILTINS[];
extern const size_t ASNIX_TRYEVAL_BUILTINS_COUNT;

#endif /* ASNIX_BUILTINS_TRYEVAL_H */
