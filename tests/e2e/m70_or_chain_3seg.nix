let a = { x = { y = { z = 5; }; }; }; in [ (a.x.y.z or "d") (a.q.y.z or "d") (a.x.q.z or "d") (a.x.y.q or "d") ]
