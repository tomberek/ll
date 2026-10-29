/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_BUILTINS_JSON_H
#define LL_BUILTINS_JSON_H

#include <stddef.h>
#include <stdint.h>

#include "builtins_misc.h" /* shared LlBuiltinMiscDef {name, build()} shape */

/*
 * `builtins.fromJSON` -- M51. Own translation unit, same rationale as
 * builtins_toml.c's fromTOML parser: a large-enough, self-contained
 * feature (a real recursive-descent JSON parser) deserves its own file
 * rather than crowding an unrelated module. Unlike fromTOML, real Nix
 * does NOT expose this one as a bare root-scope global (confirmed
 * directly: `nix-instantiate --eval -E 'fromJSON "1"'` -> "undefined
 * variable 'fromJSON'", only `builtins.fromJSON` works) -- so there's no
 * `ll_make_fromJSON_primop`-style bare wrapper here, unlike
 * builtins_toml.h's `ll_make_fromTOML_primop`.
 */

extern const LlBuiltinMiscDef LL_JSON_BUILTINS[];
extern const size_t LL_JSON_BUILTINS_COUNT;

/* M80: for derivation.c's own `__json` attribute -- a raw JSON string
 * handed to `derivation {...}` directly, which real Nix re-validates and
 * NORMALIZES (parse, then re-dump) via `StructuredAttrs::parse`/
 * `unparse()` rather than storing verbatim. Reuses this file's own
 * recursive-descent parser but with a permissive integer-overflow policy
 * (see builtins_json.c's own `permissive_overflow` comment) matching
 * that function's plain-`nlohmann::json::parse` DOM semantics, which
 * differ from `builtins.fromJSON`'s stricter SAX-visitor-based one.
 * Fatal (exit 1, asnix's usual error convention) on malformed JSON --
 * the caller doesn't need to catch anything. */
Value ll_json_parse_structured_attrs(const char *bytes, int64_t len);

#endif /* LL_BUILTINS_JSON_H */
