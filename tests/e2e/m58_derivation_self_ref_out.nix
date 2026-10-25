let
  d = derivation {
    name = "x";
    system = "x86_64-linux";
    builder = "/bin/sh";
  };
in builtins.typeOf d.out == "set" && d.out.outPath == d.outPath && d.out.outputName == "out"
