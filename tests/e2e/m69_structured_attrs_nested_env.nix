let
  d = derivation {
    name = "y";
    system = "x86_64-linux";
    builder = "/bin/sh";
    __structuredAttrs = true;
    env = {
      nested = { a = 1; b = [ 1 "two" false ]; c = { d = [ ]; }; };
    };
  };
in
  d.drvPath == "/nix/store/g19rr5ddnwsaf9yx7dbsvkj7p1k3gl08-y.drv"
  && d.outPath == "/nix/store/ll3xfky443p41yxw4nv7i1f98j6d55ah-y"
