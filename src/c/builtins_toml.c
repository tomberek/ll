/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "builtins_toml.h"

#include <stdint.h>

#include "builtins_shared.h"
#include "toml.h"
#include "value_util.h"

static Value prim_fromTOML(const Value *args, int64_t argc) {
    (void)argc;
    const char *bytes;
    int64_t len;
    ll_bshared_string_bytes(args[0], &bytes, &len, "fromTOML");
    return ll_toml_parse(bytes, (size_t)len);
}

static Value build_fromTOML(void) { return ll_bshared_make_primop((void *)prim_fromTOML, 1); }

Value ll_make_fromTOML_primop(void) { return ll_bshared_make_primop((void *)prim_fromTOML, 1); }

const LlBuiltinMiscDef LL_TOML_BUILTINS[] = {
    { "fromTOML", build_fromTOML },
};

const size_t LL_TOML_BUILTINS_COUNT = sizeof(LL_TOML_BUILTINS) / sizeof(LL_TOML_BUILTINS[0]);
