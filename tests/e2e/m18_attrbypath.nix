[
  (builtins.attrByPath [ "a" "b" ] 99 { a = { b = 42; }; })
  (builtins.attrByPath [ "a" "b" ] 99 { a = { }; })
  (builtins.attrByPath [ ] 99 { a = 1; })
]
