# The with-expression can be a variable/complex expression, not just a
# literal attrset literal.
let mk = n: { x = n; }; in with (mk 9); x
