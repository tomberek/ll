let
  a = { x = 1; };
in
  builtins.unsafeGetAttrPos "y" a == null
