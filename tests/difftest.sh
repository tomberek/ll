#!/usr/bin/env bash
# Differential test: runs each expression through `ll` (this project's LLVM
# backend) and asnix (the asm backend), and reports any disagreement. Run
# inside `nix develop` (needs $LLVM_CONFIG's lib dir on LD_LIBRARY_PATH, set
# by this project's flake.nix shellHook). ASNIX_BIN defaults to the sibling
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

    # Strip each binary's own "ll: "/"asnix: " fatal-message prefix, and
    # normalize the one message that also names itself mid-sentence
    # ("...out of ll's/asnix's representable range...") before comparing.
    # Error messages are compared by CLASS, not exact wording, since each
    # binary legitimately names itself and these self-references
    # otherwise differ by design.
    local norm_ll="${got_ll#ll: }" norm_asnix="${got_asnix#asnix: }"
    norm_ll="${norm_ll//ll\'s/X\'s}"
    norm_asnix="${norm_asnix//asnix\'s/X\'s}"

    if [ "$norm_ll" = "$norm_asnix" ]; then
        echo "OK   $expr"
    else
        echo "FAIL $expr -- ll='$got_ll' asnix='$got_asnix'"
        fail=$((fail + 1))
    fi
}

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

check 'let x = 1; in x'
check 'let x = 1; in x + 1'
check 'let x = 5; y = 10; in x + y'
check 'let x = 1; in let y = 2; in x + y'

check '(a: a + 1) 5'
check '(a: b: a + b) 3 4'
check 'let f = a: a + 1; in f 5'
check 'let x = if 1 < 2 then 10 else 20; in x + 1'
check 'let f = a: a * 2; in f (f 3)'
check '(n: if n == 0 then 0 else 99) 1'
check '(n: if n == 0 then 0 else 99) 0'
check 'let fact = n: if n == 0 then 1 else n * fact (n - 1); in fact 5'
check 'let fib = n: if n < 2 then n else fib (n - 1) + fib (n - 2); in fib 10'

check 'if isNull null then 1 else 0'
check 'if isNull 5 then 1 else 0'
check '(f: f 10) (a: a + 5)'

check '{a=1;b=2;}.a'
check '{a=1;b=2;}.b'
check 'let s = {x=5;y=10;}; in s.x + s.y'
check '{a={b=42;};}.a.b'
check 'builtins.length [1 2 3]'
check 'builtins.add 1 2'
check 'builtins.elemAt [10 20 30] 1'

echo "---"
echo "$((total - fail))/$total passed"
exit $((fail > 0))
