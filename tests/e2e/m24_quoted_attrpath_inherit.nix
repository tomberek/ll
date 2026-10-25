let
  self = { trivial = { "or" = 1; and = 2; }; };
  result =
    let
      inherit (self.trivial)
        "or"
        and
        ;
    in
    { a = and; };
in
result.a