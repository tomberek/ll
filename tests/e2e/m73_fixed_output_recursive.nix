let d = derivation {
  name = "x";
  system = "x86_64-linux";
  builder = "/bin/sh";
  outputHashMode = "recursive";
  outputHashAlgo = "sha256";
  outputHash = "0000000000000000000000000000000000000000000000000000000000000000";
}; in { outPath = d.outPath; drvPath = d.drvPath; }
