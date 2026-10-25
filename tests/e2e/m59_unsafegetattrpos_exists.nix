let
  a = { x = 1; y = 2; };
  pos = builtins.unsafeGetAttrPos "x" a;
in
  builtins.isAttrs pos
  && builtins.isString pos.file
  && builtins.isInt pos.line
  && builtins.isInt pos.column
  && (builtins.attrNames pos) == [ "column" "file" "line" ]
