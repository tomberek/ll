{
  a = builtins.isPath ./.;
  b = builtins.isPath "x";
  c = builtins.isString ./.;
  d = builtins.isString "x";
}
