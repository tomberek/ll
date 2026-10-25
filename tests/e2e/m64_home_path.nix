# M59: `~`-prefixed ("home") path literals -- see docs/architecture.md's
# M59 scope notes. Self-contained (no fixed-string dependence on this
# machine's actual $HOME): compares against `builtins.getEnv "HOME"`
# instead, verified against real `nix-instantiate --eval --strict` (same
# expression, this box's real $HOME) before being checked in.
let
  home = builtins.getEnv "HOME";
in
[
  (toString ~/a/../b == home + "/a/../b") # not canonicalized: no interpolation at all
  (toString ~/./a == home + "/./a")
  (toString ~/a/.. == home + "/a/..")
  (builtins.isPath ~/foo)
  (toString (~/foo + "/bar") == home + "/foo/bar") # `+` DOES canonicalize (M57g), unrelated to this
  (toString ~/${"a"}/../b == home + "/b") # interpolation: the WHOLE result IS canonicalized
]
