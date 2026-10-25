let f = x: g x;
    g = x: if x then 1 else f true;
in f false
