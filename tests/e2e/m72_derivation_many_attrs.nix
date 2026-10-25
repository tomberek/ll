let
  base = builtins.listToAttrs (map (i: { name = "attr${toString i}"; value = toString i; }) (builtins.genList (i: i) 100));
in (derivation (base // { name = "x"; system = "x86_64-linux"; builder = "/bin/sh"; })).outPath != ""
