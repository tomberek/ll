[
  (builtins.floor 23.5)
  (builtins.ceil 23.5)
  (builtins.floor (0 - 23.5))
  (builtins.ceil (0 - 23.5))
  (builtins.floor 23)
  (builtins.ceil 23)
  (builtins.isFloat 1.5)
  (builtins.isFloat 1)
  (builtins.isFloat (1 + 1.0))
  (builtins.typeOf 1.5)
  (builtins.typeOf (builtins.floor 1.5))
]
