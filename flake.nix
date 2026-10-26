{
  description = "ll: an LLVM-IR-backend port of asnix's Nix language evaluator";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      # inherited from asnix: the vendored arena/env/value asm is x86-64-specific.
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
            # M10: the per-invocation JIT backend (ORC LLJIT, LLVMOrc*)
            # was replaced with AOT threaded dispatch -- op handlers are
            # now plain C, compiled ONCE at build time, same as asnix's
            # own hand-written asm handlers are assembled once into
            # asnix's own binary. clang (LLVM's own compiler) is the
            # build-time tool that produces them, specifically for its
            # musttail attribute (guaranteed tail calls, the mechanism
            # that keeps a deep Nix call/force chain from growing the
            # native C stack -- see op_dispatch.h). Nothing in the
            # SHIPPED `ll` binary links against LLVM at runtime anymore
            # -- no ORC, no libLLVM, none of the shared-library-loading
            # cost M9 spent a session trimming. That whole problem was a
            # symptom of the JIT architecture, not of "using LLVM."
            #
            # llvmPackages_19, not _18: op handlers that make a real
            # (non-tail) call mid-body -- e.g. OP_MK_THUNK calling the
            # arena allocator -- need every one of the five LLOpFn
            # arguments to survive that call. Plain C has no way to ask
            # for that: the SysV ABI puts all five in caller-saved
            # registers, so the compiler spills all five around the
            # call (confirmed via objdump: 7 pushes in the compiled
            # handler vs 2 in asnix's own hand-written asm, which
            # deliberately pins 3 of its 5 state registers in
            # callee-saved ones). `preserve_none`, the calling
            # convention CPython's own tail-calling interpreter uses
            # for exactly this shape, fixes this at the type level --
            # first available in clang 19.
            llvmPackages_19.clang
          ];
          shellHook = ''
            export LL_ROOT=$(pwd)
            export ASNIX_LIBCRYPTO_LIB="$(ls ${pkgs.openssl.out}/lib/libcrypto.so* | head -1)"
            export ASNIX_MIMALLOC_LIB="${pkgs.mimalloc}/lib/libmimalloc.a"
          '';
        };
      });
    };
}
