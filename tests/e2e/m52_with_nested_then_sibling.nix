let a = { x = 1; }; b = { y = 2; }; c = { z = 3; }; in
[ (with a; with b; [ x y ]) (with c; z) ]
