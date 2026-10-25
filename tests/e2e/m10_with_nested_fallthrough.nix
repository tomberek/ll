# Inner `with` doesn't have `b` -- falls through to the outer `with`.
with { a = 1; }; with { b = 2; }; a + b
