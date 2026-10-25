let
  a = derivation { name = "a"; system = "x86_64-linux"; builder = "/bin/sh"; };
  b = derivation { name = "b"; system = "x86_64-linux"; builder = "/bin/sh"; src = a; };
in { aDrvPath = a.drvPath; aOutPath = a.outPath; bDrvPath = b.drvPath; bOutPath = b.outPath; }
