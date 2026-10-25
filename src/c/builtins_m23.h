/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef ASNIX_BUILTINS_M23_H
#define ASNIX_BUILTINS_M23_H

#include <stddef.h>
#include <stdint.h>

#include "builtins_misc.h" /* shared AsnixBuiltinMiscDef/-Builder {name, build()} shape */

/*
 * M23 builtins-expansion pass: seq, deepSeq, pathExists, readFile,
 * readFileType, readDir, toPath, getEnv, intersectAttrs, groupBy. Kept
 * in their own translation unit per this project's established
 * one-feature-batch-per-file convention for builtins modules (see
 * builtins_list2.h's own header comment for the rationale).
 *
 * All of these are true arity>=2 OR need the {name, build()} shape for
 * other reasons (readDir/readFile etc. are arity-1 but grouped here
 * anyway to keep this whole batch in one file) -- see builtins_shared.h's
 * make_curried_native, the mechanism every arity>=2 entry's build() uses.
 */

extern const AsnixBuiltinMiscDef ASNIX_M23_BUILTINS[];
extern const size_t ASNIX_M23_BUILTINS_COUNT;

/* The plain arity-1 members of this batch -- registered directly in
 * builtins.c's own {name, fn, arity} BUILTINS[] table instead of through
 * ASNIX_M23_BUILTINS above, since they don't need currying. */
Value asnix_prim_pathExists(const Value *args, int64_t argc);
Value asnix_prim_readFile(const Value *args, int64_t argc);
Value asnix_prim_readFileType(const Value *args, int64_t argc);
Value asnix_prim_readDir(const Value *args, int64_t argc);
Value asnix_prim_toPath(const Value *args, int64_t argc);
Value asnix_prim_getEnv(const Value *args, int64_t argc);
Value asnix_prim_parseDrvName(const Value *args, int64_t argc);

/* M110: exported so compiler.c's try_fused_native_call2 can fuse a
 * statically-recognized, fully-saturated `builtins.intersectAttrs a b`
 * call site into one direct native call -- see compiler.c's own
 * comment. Ordinary force-by-default native, no special handling
 * needed. */
Value asnix_prim_intersectAttrs2_impl(const Value *args, int64_t argc);

#endif /* ASNIX_BUILTINS_M23_H */
