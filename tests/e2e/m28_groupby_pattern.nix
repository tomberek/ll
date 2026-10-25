let
  foldl' = op: nul: list: if list == [] then nul else foldl' op (op nul (builtins.head list)) (builtins.tail list);
  pred = x: if x < 3 then "lo" else "hi";
in
foldl' (r: e: let key = pred e; in r // { ${key} = (r.${key} or []) ++ [e]; }) {} [1 2 3 4 5]