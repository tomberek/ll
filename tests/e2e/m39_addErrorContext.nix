[ (builtins.addErrorContext "context" 42) (let y = builtins.addErrorContext "ctx" (abort "x"); in 1) ]
