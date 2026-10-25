let f = {a, b}@args: a + b + args.a; in f { a = 1; b = 2; }
