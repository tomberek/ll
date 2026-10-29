(builtins.derivation { name = "ll-test"; system = "x86_64-linux"; builder = "/bin/sh"; args = [ "-c" "echo hi" ]; }).drvPath
