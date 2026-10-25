map (s: builtins.match "(a+)(b+)" s) [ "aab" "aaabbb" "ab" ]
