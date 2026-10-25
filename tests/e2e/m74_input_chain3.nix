let
  a = derivation { name = "a"; system = "x86_64-linux"; builder = "/bin/sh"; };
  b = derivation { name = "b"; system = "x86_64-linux"; builder = "/bin/sh"; src = a; };
  c = derivation { name = "c"; system = "x86_64-linux"; builder = "/bin/sh"; src = b; };
in { aOutPath = a.outPath; bOutPath = b.outPath; cDrvPath = c.drvPath; cOutPath = c.outPath; }
