let
  a = derivation {
    name = "a";
    system = "x86_64-linux";
    builder = "/bin/sh";
    outputHashMode = "flat";
    outputHashAlgo = "sha256";
    outputHash = "0000000000000000000000000000000000000000000000000000000000000000";
  };
  b = derivation { name = "b"; system = "x86_64-linux"; builder = "/bin/sh"; src = a; };
in { aOutPath = a.outPath; bDrvPath = b.drvPath; bOutPath = b.outPath; }
