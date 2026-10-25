# A lexical binding always wins over an enclosing `with`, even one
# introduced in an outer scope than the `with` itself.
let a = 100; in with { a = 1; }; a
