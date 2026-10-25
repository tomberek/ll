let
  a = derivation { name = "a"; system = "x86_64-linux"; builder = "/bin/sh"; };
  b = derivation {
    name = "b";
    system = "x86_64-linux";
    builder = "${a}/bin/sh";
  };
in { bDrvPath = b.drvPath; bOutPath = b.outPath; }
