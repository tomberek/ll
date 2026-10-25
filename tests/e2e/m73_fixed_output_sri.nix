let d = derivation {
  name = "x";
  system = "x86_64-linux";
  builder = "/bin/sh";
  outputHashAlgo = "";
  outputHash = "sha256-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=";
}; in { outPath = d.outPath; drvPath = d.drvPath; }
