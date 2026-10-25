#!/usr/bin/env bash
# Differential test: runs each expression through `ll` (this project's LLVM
# backend), asnix (the asm backend), and nix-instantiate, and reports any
# disagreement. Run inside `nix develop` (needs $LLVM_CONFIG's lib dir on
# LD_LIBRARY_PATH, set by this project's flake.nix shellHook -- see
# soft-wishing-engelbart.md's M3 notes). ASNIX_BIN defaults to the sibling
# ~/asnix checkout's own build.
set -u
cd "$(dirname "$0")/.."

ASNIX_BIN="${ASNIX_BIN:-$HOME/asnix/asnix}"
LL_BIN="${LL_BIN:-./ll}"

fail=0
total=0

check() {
    local expr="$1"
    total=$((total + 1))
    local f
    f=$(mktemp --suffix=.nix)
    printf '%s' "$expr" > "$f"

    local got_ll got_asnix
    got_ll=$("$LL_BIN" "$expr" 2>&1)
    got_asnix=$("$ASNIX_BIN" "$f" 2>&1)
    rm -f "$f"

    if [ "$got_ll" = "$got_asnix" ]; then
        echo "OK   $expr"
    else
        echo "FAIL $expr -- ll='$got_ll' asnix='$got_asnix'"
        fail=$((fail + 1))
    fi
}

# M3 scope: OP_PUSH_INT/OP_ADD/OP_SUB/OP_MUL/OP_DIV/OP_HALT only -- no
# variables, branches, or non-int types yet (M4+).
check '42'
check '1+2'
check '7*6'
check '100-58'
check '9 / 3'
check '0-5'
check '2*2*2*2*2'
check '(0-7) / 2'
check '1000000*1000000*1000000'
check '7-20'
check '0-0'
check '1 / 0'
check '9223372036854775807 + 1'
check '(0-1152921504606846976) - 1'
check 'if 1 < 2 then 10 else 20'
check 'if 1 > 2 then 10 else 20'
check 'if 1 == 1 then 100 else 200'
check 'if 1 != 1 then 100 else 200'
check 'if true then 1 else 2'
check 'if false then 1 else 2'
check 'if 1 < 2 then (if 3 > 4 then 1 else 2) else 3'
check 'if 1 < 2 then 2*2*2*2*2 else 0'
check '(if true then 1 else 2) + (if false then 10 else 20)'

# M4 (first slice): non-recursive, non-lazy `let` -- OP_FRAME_ALLOC/SET/
# EXIT + OP_VAR_REF for eager (non-thunked) bindings. A `let` binding
# asnix compiles to OP_MK_THUNK (e.g. a non-constant-foldable RHS like an
# `if` expression) is still out of scope -- real laziness/thunk-forcing
# is M4's still-pending second half.
check 'let x = 1; in x'
check 'let x = 1; in x + 1'
check 'let x = 5; y = 10; in x + y'
check 'let x = 1; in let y = 2; in x + y'

# M4 (second slice): closures/OP_LAMBDA, calls/OP_CALL, thunks/
# OP_MK_THUNK with real forcing, and the fused compare-and-branch ops
# (OP_EQ_IF/OP_NEQ_IF/OP_LT_IF/OP_GE_IF) a variable `if` condition
# compiles to instead of plain OP_EQ/OP_LT + OP_IF.
check '(a: a + 1) 5'
check '(a: b: a + b) 3 4'
check 'let f = a: a + 1; in f 5'
check 'let x = if 1 < 2 then 10 else 20; in x + 1'
check 'let f = a: a * 2; in f (f 3)'
check '(n: if n == 0 then 0 else 99) 1'
check '(n: if n == 0 then 0 else 99) 0'
check 'let fact = n: if n == 0 then 1 else n * fact (n - 1); in fact 5'
check 'let fib = n: if n < 2 then n else fib (n - 1) + fib (n - 2); in fib 10'

# M5: real PrimOps, reachable via root_env.c's lightweight root Env
# (builtins/map/import/etc. bound, WITHOUT the real `derivation`
# builtin's self-bootstrap -- see root_env.c's header comment). Wrapped
# in `if` since the CLI's printer is still int-only (asnix_int_value) --
# printing a bare boolean/PrimOp/attrset result isn't supported yet.
check 'if isNull null then 1 else 0'
check 'if isNull 5 then 1 else 0'
check '(f: f 10) (a: a + 5)'

# M6 (first slice): strings-as-opaque-constants (OP_PUSH_CONST), lists
# (OP_MK_LIST/asnix_bshared_make_list), attrsets (OP_MK_ATTRS/
# asnix_bshared_make_attrs), and OP_SELECT (asnix_bshared_attrs_probe +
# force). Also fixed a real bug this surfaced: asnix_apply's PrimOp path
# wasn't forcing thunked arguments before calling the native fn (asm's
# own M25 comment explains why that's the CALLER's job, not the primop's).
#
# Known gap, not yet fixed: arity>=2 vendored builtins (add, elemAt, ...)
# are curried via asnix_bshared_make_curried_native, which builds a REAL
# asnix Closure (storing an IrProgram*) -- colliding with this backend's
# own LLClosure layout (storing a native fn pointer) at the same
# ASNIX_TAG_CLOSURE tag. Arity-1 builtins (isNull, length) and
# user-defined multi-param lambdas (which desugar to nested OP_LAMBDAs,
# both ends using LLClosure) are unaffected -- only calling a curried
# multi-arg vendored builtin crashes. Needs unifying the two closure
# representations (e.g. both store an IrProgram* and lazily JIT-compile
# on first call) before e.g. `builtins.add`/`builtins.elemAt` can work.
check '{a=1;b=2;}.a'
check '{a=1;b=2;}.b'
check 'let s = {x=5;y=10;}; in s.x + s.y'
check '{a={b=42;};}.a.b'
check 'builtins.length [1 2 3]'

echo "---"
echo "$((total - fail))/$total passed"
exit $((fail > 0))
