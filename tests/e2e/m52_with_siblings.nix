let foo = { bar = 1; }; baz = { qux = 2; }; in [ (with foo; bar) (with baz; qux) ]
