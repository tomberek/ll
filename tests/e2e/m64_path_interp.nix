# M59: interpolated path literals (`./${foo}`, `/${foo}`, ...) -- see
# docs/architecture.md's M59 scope notes. Compares each interpolated form
# against an equivalent plain literal rather than printing an absolute
# path directly, so the expected output doesn't depend on where this repo
# happens to be checked out (both sides resolve against this same file's
# directory). Verified against real `nix-instantiate --eval --strict`
# (same expression) before being checked in.
let
  foo = "foo";
in
[
  (./${foo} == ./foo)
  (./a-${foo}-b == ./a-foo-b)
  (/${foo} == /foo)
  (./${foo + "/bar"} == ./foo/bar)
  (./bar/${foo} == ./bar/foo)
  (/${foo}/${"bar"} == /foo/bar)
]
