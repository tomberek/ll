let
  maxInt = 1152921504606846975;      # LL_INT_MAX (2^60 - 1)
  a = 1000000;
  b = 2000000;
in [
  maxInt
  (a + b) (a - b) (a * b) (a / b)
  (builtins.add a b) (builtins.sub a b) (builtins.mul a b) (builtins.div a b)
]
