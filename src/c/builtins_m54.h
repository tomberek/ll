/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef ASNIX_BUILTINS_M54_H
#define ASNIX_BUILTINS_M54_H

#include <stddef.h>

#include "asnix/value.h"

/*
 * M54: a batch of previously entirely-missing builtins found by a
 * conformance-triage pass (`attribute 'X' missing, aborting`):
 * `bitAnd`/`bitOr`/`bitXor`, `trace`, `zipAttrsWith`, `toXML`,
 * `convertHash`, `getContext`/`appendContext`, `scopedImport`. Kept in
 * its own translation unit, same "no textual collision with a sibling
 * agent's own builtins module" rationale as every prior builtins-
 * expansion pass (see builtins_misc.h/builtins_m23.h/etc). Each entry's
 * `build` is called once, after asnix_arena_init() has run, same
 * lazy-build-at-use pattern as ASNIX_BUILTINS_MISC. */
typedef Value (*AsnixBuiltinM54Builder)(void);

typedef struct {
    const char *name;
    AsnixBuiltinM54Builder build;
} AsnixBuiltinM54Def;

extern const AsnixBuiltinM54Def ASNIX_M54_BUILTINS[];
extern const size_t ASNIX_M54_BUILTINS_COUNT;

/* `scopedImport` is one of real Nix's bare root-scope globals (confirmed
 * via cppnix source, `~/nix/src/libexpr/primops.cc`: `.name =
 * "scopedImport"`, no `__` prefix -- unlike almost everything else in
 * this batch) -- see eval_file.c's root Env / compiler.c's
 * init_builtins_frame. Wraps the exact same curried native
 * `builtins.scopedImport` uses. */
Value asnix_make_scopedImport_primop(void);

#endif /* ASNIX_BUILTINS_M54_H */
