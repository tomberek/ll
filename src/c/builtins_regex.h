/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_BUILTINS_REGEX_H
#define LL_BUILTINS_REGEX_H

#include <stddef.h>

#include "builtins_misc.h" /* shared LlBuiltinMiscDef/-Builder {name, build()} shape */

/*
 * `builtins.match`/`builtins.split` -- POSIX Extended Regular Expression
 * (ERE) support, built on glibc's <regex.h> (`regcomp`/`regexec`/
 * `regfree`) for the common case -- it's both correct and fast for any
 * pattern with no capture groups and none of a handful of bracket-
 * expression extensions glibc can't be trusted for -- plus
 * `regex_engine.c`, a small hand-rolled POSIX ERE backtracking matcher
 * (M68), used whenever a pattern actually needs it: to recompute
 * capture groups (and, for `split`, sometimes the match boundary
 * itself) the way real Nix's libstdc++ `std::regex` would, which
 * glibc's regexec doesn't always agree with for a specific, identified
 * family of ambiguous/repeated subexpressions -- see regex_engine.c's
 * own header comment and docs/architecture.md's M68 scope notes for the
 * full story (M55's scope notes cover the triage that preceded it).
 * See builtins_regex.c for the match/split algorithms and routing
 * logic themselves.
 */

extern const LlBuiltinMiscDef LL_REGEX_BUILTINS[];
extern const size_t LL_REGEX_BUILTINS_COUNT;

#include "ll/value.h" /* Value */
#include <stdint.h>       /* int64_t */

/* M110: exported so compiler.c's try_fused_native_call2 can fuse a
 * statically-recognized, fully-saturated `builtins.match a b` call site
 * into one direct native call -- see compiler.c's own comment. Ordinary
 * force-by-default native, no special handling needed. */
Value ll_prim_match2_impl(const Value *args, int64_t argc);

#endif /* LL_BUILTINS_REGEX_H */
