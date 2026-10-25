(builtins.derivation { name = "asnix-test"; system = "x86_64-linux"; builder = "/bin/sh"; args = [ "-c" "echo hi" ]; }).outPath
