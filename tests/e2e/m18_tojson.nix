[
  (builtins.toJSON { a = 1; b = [ 1 2 "x\"y" ]; c = null; d = true; e = false; })
  (builtins.toJSON "a\nb\tc\\d")
  (builtins.toJSON [ ])
  (builtins.toJSON { })
  (builtins.toJSON (0 - 5))
]
