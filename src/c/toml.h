/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef ASNIX_TOML_H
#define ASNIX_TOML_H

#include <stddef.h>

#include "asnix/value.h"

/*
 * A hand-written, from-scratch recursive-descent TOML parser -- M23,
 * built for `builtins.fromTOML` (see docs/architecture.md's "M23 scope
 * notes" for the exact subset implemented and what's deliberately cut).
 *
 * `asnix_toml_parse` is the sole entry point: parses `text[0..len)` as a
 * complete TOML document and returns it as an ordinary asnix attrset
 * Value tree (built via builtins_shared.c's asnix_bshared_make_attrs/
 * asnix_bshared_make_list/asnix_make_string_value/asnix_make_int, exactly
 * like every other builtin that constructs aggregate values -- no
 * bespoke heap layout code here). Fatal (fprintf+exit, matching every
 * other builtin's error convention in this codebase -- asnix has no
 * exception/tryEval mechanism to catch a parse error with anyway) on any
 * syntax error OR on hitting a construct this subset deliberately does
 * not support (TOML floats, `inf`/`nan` -- asnix's Value representation
 * is integer-only, see include/asnix/value.h; there is no way to
 * represent a float without silently producing a wrong value, which is
 * worse than a clear error).
 */
Value asnix_toml_parse(const char *text, size_t len);

#endif /* ASNIX_TOML_H */
