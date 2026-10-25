# M184 only touches the non-bracket-extension, non-needs_custom path of
# asnix_prim_match2_impl -- confirms the pre-existing
# asnix_regex_needs_custom_engine full-bypass route (equivalence class
# here) is completely unaffected, repeated several times to exercise
# the regex cache the same way the other M184 fixtures do.
map (s: builtins.match "[[=a=]]+" s) [ "aaa" "AAA" "b" "" "aAa" ]
