let
  attrs = { a = "x"; };
  g = x: builtins.getAttr "a" x;
in
(g attrs) + "y"
