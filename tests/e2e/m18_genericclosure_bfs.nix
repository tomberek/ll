builtins.genericClosure {
  startSet = [ { key = 1; } ];
  operator = item:
    if item.key == 1 then [ { key = 2; } { key = 3; } ]
    else if item.key == 2 then [ { key = 4; } ]
    else if item.key == 3 then [ { key = 5; } ]
    else [ ];
}
