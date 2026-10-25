let p = "(a+)(b+)"; in [ (builtins.match p "aabbb") (builtins.split p "xaabbbxaabbbx") ]
