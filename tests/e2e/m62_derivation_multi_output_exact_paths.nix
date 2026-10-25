let
  d = derivation {
    name = "x";
    system = "x86_64-linux";
    builder = "/bin/sh";
    outputs = [ "out" "bin" ];
  };
in
  d.outPath == "/nix/store/i1ig6pkdxq7h8v8q9lfasrpmpv5fkd1c-x"
  && d.bin.outPath == "/nix/store/vbb7drd5yyazi29lq4yqdljm9gkvfi85-x-bin"
