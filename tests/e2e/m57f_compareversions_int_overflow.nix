let c = builtins.compareVersions; in [
  (c "2147483648" "9")
  (c "2147483647" "9")
  (c "0000000000000000000002" "1")
]
