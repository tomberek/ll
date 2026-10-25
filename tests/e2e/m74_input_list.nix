let
  a = derivation { name = "a"; system = "x86_64-linux"; builder = "/bin/sh"; };
  b = derivation { name = "b2"; system = "x86_64-linux"; builder = "/bin/sh"; };
  c = derivation {
    name = "c";
    system = "x86_64-linux";
    builder = "/bin/sh";
    nativeBuildInputs = [ a b ];
  };
in { cDrvPath = c.drvPath; cOutPath = c.outPath; }
