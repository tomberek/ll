let d = derivation {
  name = "x";
  system = "x86_64-linux";
  builder = "/bin/sh";
  outputHashAlgo = "md5";
  outputHash = "00000000000000000000000000000000";
}; in { outPath = d.outPath; drvPath = d.drvPath; }
