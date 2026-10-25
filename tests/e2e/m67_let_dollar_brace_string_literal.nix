let
  ${"a"} = 1;
  attrs = rec {
    b = c;
    ${"c"} = d;
    d = a;
  };
in
{
  inherit ${"a"};
  inherit attrs;
  inherit (attrs) ${"b"} ${"c"} d;
}
