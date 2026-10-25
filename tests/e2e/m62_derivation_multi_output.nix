let
  d = derivation {
    name = "x";
    system = "x86_64-linux";
    builder = "/bin/sh";
    outputs = [ "out" "bin" ];
  };
in
  builtins.typeOf d.bin == "set"
  && d.out.outPath == d.outPath
  && d.bin.outPath != d.outPath
  && d.bin.outputName == "bin"
