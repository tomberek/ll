/* M138: a bare URI literal whose scheme contains '.' and '+' -- RFC 3986
 * scheme chars, real Nix's own URI lexer rule accepts them (see
 * src/c/lexer.c's uri_scheme_char/try_scan_uri). This exercises the exact
 * edge case M129/M138 flagged: ident_cont stops at '.'/'+' (they aren't
 * identifier characters), but uri_scheme_char does NOT -- so this must
 * still lex as ONE TOK_URI token, not an identifier run followed by
 * something else. Checked both as a standalone scheme and inside a list
 * alongside an ordinary attribute-selection chain that looks similar
 * lexically (dots) but must NOT be treated as a path/URI. */
[
  a.b+c:foo
  x+.y-z:tail
  (let pkgs = { hello = { outPath = "OUT"; }; }; in pkgs.hello.outPath)
]
