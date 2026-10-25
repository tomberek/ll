let a = {}; d = { y = 5; w = 6; }; in builtins.attrNames (a.x.y or d)
