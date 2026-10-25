[
  (builtins.replaceStrings [ "a" "b" ] [ "A" "B" ] "abc")
  (builtins.replaceStrings [ "" ] [ "X" ] "ab")
  (builtins.replaceStrings [ "a" ] [ "aa" ] "banana")
]
