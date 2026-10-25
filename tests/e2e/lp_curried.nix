let outer = {a}: {b}: a + b; in (outer { a = 1; }) { b = 2; }
