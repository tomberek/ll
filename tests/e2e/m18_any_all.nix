[ (builtins.any (x: x > 2) [ 1 2 3 ]) (builtins.all (x: x > 0) [ 1 2 3 ]) (builtins.any (x: x > 5) [ 1 2 3 ]) (builtins.all (x: x > 1) [ 1 2 3 ]) ]
