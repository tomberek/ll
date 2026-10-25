let k = "x"; a = { x = { y = 7; }; }; in [ (a.${k}.y or "d") (a.nope.y or "d") (a.${k}.nope or "d") ]
