let
  a = derivation {
    name = "a";
    system = "x86_64-linux";
    builder = "/bin/sh";
    outputs = [ "out" "dev" ];
  };
  b = derivation { name = "b"; system = "x86_64-linux"; builder = "/bin/sh"; src = a.dev; };
in { aOut = a.out.outPath; aDev = a.dev.outPath; bDrvPath = b.drvPath; bOutPath = b.outPath; }
