let lib_attrByPath = attrPath: default: set:
  let go = n: s:
    if n == builtins.length attrPath then s
    else let attr = builtins.elemAt attrPath n;
         in if s ? ${attr} then go (n+1) s.${attr} else default;
  in go 0 set;
in lib_attrByPath (map (x: x) ["a" "b"]) 99 { a = { b = 7; }; }
