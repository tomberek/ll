#ifndef LL_PRINT_H
#define LL_PRINT_H

#include "ll/value.h"

/* Prints `v` the way asnix's own main.c printer does (quoted strings
 * with Nix's escaping rules, bare paths, `[ ]`/`{ }` for lists/attrs
 * sorted by name, <PRIMOP>/<LAMBDA>/<PRIMOP-APP> for callables,
 * «repeated» cycle detection), followed by a newline. Ported verbatim
 * from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445's src/c/main.c. */
void ll_print_value(Value v);

#endif /* LL_PRINT_H */
