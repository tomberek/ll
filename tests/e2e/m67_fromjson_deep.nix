let
  rep = c: n: builtins.concatStringsSep "" (builtins.genList (_: c) n);
  deep = n: builtins.length (builtins.fromJSON (rep "[" n + rep "]" n));
in
deep 50000
