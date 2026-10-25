builtins.genericClosure {
  startSet = [ { key = 1; name = "a"; } ];
  operator = item:
    if item.key == 1 then [ { key = 2; name = "b"; } ] else [ ];
}
