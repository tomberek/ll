builtins.genericClosure {
  startSet = [ { key = "a"; } ];
  operator = x: if x.key == "a" then [ { key = "b"; } ] else [ ];
}
