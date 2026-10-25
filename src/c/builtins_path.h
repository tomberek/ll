/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef ASNIX_BUILTINS_PATH_H
#define ASNIX_BUILTINS_PATH_H

#include <stddef.h>

#include "builtins_misc.h" /* AsnixBuiltinMiscDef -- same {name,build()} shape precedent
                             * builtins_shared.h's own header comment describes. */

/* M80: `builtins.path`/`builtins.filterSource` -- see builtins_path.c's
 * own header comment for the full picture. */
extern const AsnixBuiltinMiscDef ASNIX_PATH_BUILTINS[];
extern const size_t ASNIX_PATH_BUILTINS_COUNT;

#endif /* ASNIX_BUILTINS_PATH_H */
