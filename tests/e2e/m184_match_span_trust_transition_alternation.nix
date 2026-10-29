# M184: `(aa|a)(aa|a)` is exactly M68's quirk-1 example -- real Nix's
# libstdc++ picks the first-ordered branch that completes ("aa" then
# "a" for "aaa"), not the longest. This doesn't affect WHETHER a whole
# match exists (see docs/architecture.md's M184 section for why), but
# it's the sharpest available probe of "does the custom engine's own
# existence verdict, once trusted and used WITHOUT regexec() at all,
# still agree with real Nix" -- 12 subjects, more than
# LL_REGEX_SPAN_VERIFY_CALLS (8), so this exercises calls both
# during the verification window (glibc-driven, as before) AND after
# the trust flip (custom-engine-driven, the new M184 fast path), with
# the LAST three subjects repeating the FIRST three to directly confirm
# the pre-/post-trust answers for identical inputs don't change.
map (s: builtins.match "(aa|a)(aa|a)" s) [
  "aaa" "aaaa" "aa" "a" "" "aaaaa" "b" "aaaaaa"
  "aaaaaaa" "baaa" "aaab" "aaaaaaaa"
  "aaa" "aa" "a"
]
