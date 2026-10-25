# Inner `with` has the same key as the outer one -- inner wins.
with { a = 1; }; with { a = 2; }; a
