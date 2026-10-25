let
  d = derivation {
    name = "x";
    system = "x86_64-linux";
    builder = "/bin/sh";
    __structuredAttrs = true;
    env = { foo = "bar"; };
  };
in
  d.drvPath == "/nix/store/3g9b3db4qr9ybd2mjhwxmf3d51xn4rbl-x.drv"
  && d.outPath == "/nix/store/85k3fnwmzp95ccbwmn2ixisp0y8gdqjj-x"
