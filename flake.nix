{
  description = "ll: an LLVM-IR-backend port of asnix's Nix language evaluator";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      # the vendored arena/env/value asm is x86-64-specific.
      systems = [ "x86_64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (s: f nixpkgs.legacyPackages.${s});
      # openssl's own unversioned "libcrypto.so" symlink isn't guaranteed to
      # exist in every nixpkgs revision's `.out` output; find whatever
      # libcrypto.so* is actually there at eval time, the same thing the
      # devShell's shellHook computes at shell-entry time via `ls`.
      libcryptoSoName = pkgs:
        nixpkgs.lib.lists.findFirst (n: nixpkgs.lib.hasPrefix "libcrypto.so" n)
          (throw "ll flake: no libcrypto.so* found in ${pkgs.openssl.out}/lib")
          (builtins.attrNames (builtins.readDir "${pkgs.openssl.out}/lib"));
    in {
      packages = forAllSystems (pkgs: rec {
        ll = pkgs.stdenv.mkDerivation {
          pname = "ll";
          version = "0.1.0";
          src = self;
          nativeBuildInputs = [ pkgs.llvmPackages_19.clang pkgs.makeWrapper ];
          buildInputs = [ pkgs.mimalloc pkgs.openssl ];
          # LL_MIMALLOC_LIB is consumed by the Makefile at BUILD time only
          # (statically links mimalloc's archive into the binary).
          # LL_LIBCRYPTO_LIB is read at RUNTIME via getenv (hash_shim.c
          # dlopen's libcrypto -- nixpkgs' openssl has no single
          # predictable, ambient-search-path-reachable location), so it
          # also needs to end up wrapped onto the installed binary below.
          LL_MIMALLOC_LIB = "${pkgs.mimalloc}/lib/libmimalloc.a";
          buildPhase = ''
            runHook preBuild
            make CC=${pkgs.llvmPackages_19.clang}/bin/clang
            runHook postBuild
          '';
          installPhase = ''
            runHook preInstall
            mkdir -p $out/bin
            cp ll $out/bin/.ll-unwrapped
            makeWrapper $out/bin/.ll-unwrapped $out/bin/ll \
              --set LL_LIBCRYPTO_LIB "${pkgs.openssl.out}/lib/${libcryptoSoName pkgs}"
            runHook postInstall
          '';
        };
        default = ll;
      });

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
