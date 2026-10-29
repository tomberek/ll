/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_TOML_H
#define LL_TOML_H

#include <stddef.h>

#include "ll/value.h"

/*
 * A hand-written, from-scratch recursive-descent TOML parser -- M23,
 * built for `builtins.fromTOML` (see docs/architecture.md's "M23 scope
 * notes" for the exact subset implemented and what's deliberately cut).
 *
 * `ll_toml_parse` is the sole entry point: parses `text[0..len)` as a
 * complete TOML document and returns it as an ordinary asnix attrset
 * Value tree (built via builtins_shared.c's ll_bshared_make_attrs/
 * ll_bshared_make_list/ll_make_string_value/ll_make_int, exactly
 * like every other builtin that constructs aggregate values -- no
 * bespoke heap layout code here). Fatal (fprintf+exit, matching every
 * other builtin's error convention in this codebase -- asnix has no
 * exception/tryEval mechanism to catch a parse error with anyway) on any
 * syntax error OR on hitting a construct this subset deliberately does
 * not support (TOML floats, `inf`/`nan` -- asnix's Value representation
 * is integer-only, see include/ll/value.h; there is no way to
 * represent a float without silently producing a wrong value, which is
 * worse than a clear error).
 */
Value ll_toml_parse(const char *text, size_t len);

#endif /* LL_TOML_H */
