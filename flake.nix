{
  description = "ll: an LLVM-IR-backend port of asnix's Nix language evaluator";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      # the vendored arena/env/value asm is x86-64-specific.
      systems = [ "x86_64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (s: f nixpkgs.legacyPackages.${s});
    in {
      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          packages = with pkgs; [
            binutils
            gcc
            gdb
            valgrind
            openssl
            mimalloc
            entr
            # clang must be 19+: op handlers that make a non-tail call
            # mid-body need the `preserve_none` calling convention,
            # first available in clang 19.
            llvmPackages_19.clang
            # llvm-profdata, matching major version, for merging PGO
            # instrumentation data -- NOT bundled with the `clang`
            # package above.
            llvmPackages_19.llvm
            # lld, for `make ll-flto` (full LTO): neither gold nor bfd
            # carries the LLVM bitcode LTO plugin.
            llvmPackages_19.lld
          ];
          shellHook = ''
            export LL_ROOT=$(pwd)
            export LL_LIBCRYPTO_LIB="$(ls ${pkgs.openssl.out}/lib/libcrypto.so* | head -1)"
            export LL_MIMALLOC_LIB="${pkgs.mimalloc}/lib/libmimalloc.a"
          '';
        };
      });
    };
}
