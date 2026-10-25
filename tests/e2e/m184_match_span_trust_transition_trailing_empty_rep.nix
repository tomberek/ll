# M184: `(a*)*` is M68's quirk-2 example (a repeated group's trailing
# zero-width iteration overwrites the last real iteration's capture,
# real Nix: `(a*)*` on "aa" -> `[ "" ]`, not `[ "aa" ]`). Same
# verify-then-trust stress shape as the alternation fixture above: 10
# subjects, past the 8-call verification threshold.
map (s: builtins.match "(a*)*" s) [
  "aa" "" "aaa" "a" "aaaa" "b" "baa" "aaaaa" "" "aa"
]
