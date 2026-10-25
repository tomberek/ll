# A `let` introduced *inside* the `with`'s body still shadows it.
with { a = 1; }; let a = 2; in a
