let f = args@{a, b}: a + b + args.a; in f { a = 1; b = 2; }
