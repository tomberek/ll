let bString = "b"; in
(rec { "${bString}" = a; a = true; }).b
