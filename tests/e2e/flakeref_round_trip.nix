map (s: builtins.flakeRefToString (builtins.parseFlakeRef s) == s) [
  "github:NixOS/nixpkgs"
  "flake:nixpkgs"
  "path:/foo/bar"
  "git+https://example.com/repo?ref=main"
  "https://example.com/a.tar.gz"
]
