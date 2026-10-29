/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_AST_H
#define LL_AST_H

#include <stddef.h>
#include <stdint.h>

/* `let` is genuinely recursive (letrec): every binding in one
 * `let ... in` is visible to itself and every sibling, regardless of
 * source order. AST_LET carries its whole binding list (like
 * AST_ATTRSET), not just one name -- see compiler.c's AST_LET case.
 * `rec { ... }` gives the same self/sibling visibility
 * (AST_ATTRSET.is_rec); plain `{ ... }` does not. */

typedef enum {
    AST_INT,
    AST_FLOAT,
    AST_BOOL,
    AST_NULL,
    AST_STRING,
    AST_VAR,
    AST_LET,
    AST_IF,
    AST_LAMBDA,
    AST_APP,
    AST_BINOP,
    AST_LIST,
    AST_ATTRSET,
    AST_SELECT,
    AST_AND,    /* short-circuit `&&`: as.logic */
    AST_OR,     /* short-circuit `||`: as.logic */
    AST_NOT,    /* unary `!`: as.not1 */
    AST_ASSERT, /* `assert cond; body`: as.assert1 */
    AST_WITH,
    AST_HAS_ATTR,
    AST_PATH,
    AST_CURPOS, /* `__curPos` -- see parser.c's special-case in its TOK_IDENT
                 * handling and compiler.c's own case for how `line`/`column` (captured
                 * here, at parse time) become a `{file;line;column;}` attrset. */
} AstKind;

typedef struct Ast Ast;

/* One piece of an AST_STRING literal: either a literal chunk of text
 * (escapes already decoded, and -- for a '' ... '' literal -- indentation
 * already stripped by the time the parser builds this) or an interpolated
 * expression ("${ ... }"). Plain-malloc'd, same rationale as AstBinding
 * below. */
typedef struct {
    int is_expr;
    const char *text;   /* valid when !is_expr */
    size_t text_len;
    Ast *expr;           /* valid when is_expr */
} AstStringPart;

/* One formal in a lambda-pattern (`{a, b ? default, ...}: body`) parameter
 * list. Plain-malloc'd, same rationale as AstBinding below. `default_`
 * is NULL for a required field (no `? expr`). */
typedef struct {
    const char *name;
    size_t name_len;
    Ast *default_;
} AstFormal;

/* One binding in a `let`/`rec {}`/`{}` binding list. Plain-malloc'd (not
 * arena-allocated): it's a compile-time-only auxiliary array, not part of
 * the runtime Value graph the arena exists to serve -- same rationale as
 * IrBuilder's growable arrays in irbuild.c.
 *
 * `inherit x;` / `inherit (expr) x y;` desugar at parse time (parser.c)
 * into ordinary bindings whose `value` is an AST_VAR(x) (bare form) or
 * AST_SELECT(expr, x) (`(expr)` form). Only the BARE form sets
 * `is_inherit`: verified against real nix-instantiate, `inherit (expr)
 * x;`'s `expr` resolves against the enclosing block's own (possibly
 * recursive) scope just like any ordinary binding's value -- siblings
 * are visible (`let inherit (pt) x; pt = {...}; in ...` sees `pt`) -- so
 * it needs no special treatment. A bare `inherit x;`, however, must
 * resolve `x` against the scope *enclosing* the let/rec/attrset, never
 * against that construct's own scope, so `rec { inherit x; }` can't
 * accidentally self-reference the `x` it's trying to pull in from
 * outside (also verified against real nix-instantiate). */
typedef struct {
    const char *name;
    size_t name_len;
    Ast *value;
    int is_inherit;
    /* Set only on the synthetic hidden binding `inherit (expr) a b c;`
     * (parser.c's parse_binding_stmt) generates to hold `expr` itself,
     * shared by every one of that clause's `AST_SELECT`s. Gets a normal
     * frame/env slot like any other binding, but is never a real
     * attribute: both AST_ATTRSET compiler cases (compiler.c) and the
     * pretty-printer must skip it. */
    int is_hidden;
} AstBinding;

/* One `${expr} = value;` entry in a plain `{...}`/`rec {...}` literal --
 * see AST_ATTRSET's own `dyn_bindings` field comment. Plain-malloc'd,
 * same rationale as AstBinding above. */
typedef struct {
    Ast *name_expr;
    Ast *value;
} AstDynBinding;

struct Ast {
    AstKind kind;
    /* Byte offset of this node's leading token in the source buffer, set
     * by ll_ast_new_at (parser.c's own construction sites) -- 0 for
     * any node built via the plain ll_ast_new (compiler.c's synthetic
     * desugared nodes have no single corresponding source span). Exists
     * purely so a debug tool (irdump) can map a compiled op back to an
     * approximate source line for a human auditing which Nix constructs
     * trigger which optimizer passes -- never read by compile_expr or
     * any other part of the real eval path. */
    uint32_t src_pos;
    union {
        int64_t int_val;   /* AST_INT */
        double float_val;  /* AST_FLOAT */
        int bool_val;       /* AST_BOOL */
        struct {
            const char *name;
            size_t name_len;
        } var; /* AST_VAR */
        struct {
            AstBinding *bindings;
            size_t count;
            Ast *body;
        } let1; /* AST_LET */
        struct {
            Ast *cond, *then_, *else_;
        } if1; /* AST_IF */
        struct {
            const char *param;    /* plain-identifier form (`a: body`); unused when is_pattern */
            size_t param_len;
            Ast *body;
            int is_pattern;        /* 1 for `{...}: body` / `{...}@n: body` / `n@{...}: body` */
            /* Pattern-only fields below (see docs/architecture.md's lambda-patterns
             * notes): a destructuring lambda is still arity-1 at the OP_CALL ABI
             * level -- the compiler desugars the pattern into synthetic
             * AST_IF/AST_HAS_ATTR/AST_SELECT nodes over the one real attrset
             * argument (see compiler.c's AST_LAMBDA case). */
            AstFormal *formals;
            size_t formals_count;
            int has_ellipsis;      /* `...` present: extra fields beyond `formals` are allowed */
            int has_as_name;       /* an `@name` alias was given, either position */
            const char *as_name;
            size_t as_name_len;
        } lambda; /* AST_LAMBDA */
        struct {
            Ast *fn, *arg;
        } app; /* AST_APP */
        struct {
            /* '+' '-' '*'                  arithmetic
             * 'e' '==' 'n' '!='            equality/inequality
             * 'l' '<' 'g' '>' 'L' '<=' 'G' '>='   ordering (INT-only, see compiler.c)
             * 'u' '//'                     attrset update/merge
             * 'c' '++'                     list concatenation
             * '/' '/'                      integer division -- the char code and the
             *                              source operator happen to coincide here, unlike
             *                              every other op letter above */
            char op;
            Ast *lhs, *rhs;
        } binop; /* AST_BINOP */
        struct {
            AstStringPart *parts;
            size_t count;
        } string; /* AST_STRING */
        struct {
            Ast **elems;
            size_t count;
        } list; /* AST_LIST */
        struct {
            AstBinding *bindings;
            size_t count;
            int is_rec; /* `rec { ... }` vs plain `{ ... }` */
            AstDynBinding *dyn_bindings; /* `${expr} = value;` entries -- NULL/0 unless this
                                            * literal had any (plain `{...}`/`rec {...}` only;
                                            * `let` rejects these at parse time, see parser.c's
                                            * parse_binding_stmt). Applied at runtime, after
                                            * `bindings` above is fully built -- see compiler.c's
                                            * AST_ATTRSET case and OP_INSERT_DYNAMIC_ATTR (ir.h).
                                            * A dynamic binding never gets its OWN ScopeFrame
                                            * slot, so a *static* sibling still can't reference a
                                            * dynamically-NAMED attribute (confirmed: `rec {
                                            * ${k}=1; b=a; }` with `k="a"` still errors "undefined
                                            * variable 'a'") -- but a dynamic binding's own
                                            * name/value expression DOES read the enclosing
                                            * `rec {}`'s frame (so it CAN see a *static* sibling:
                                            * `let bString="b"; in (rec { "${bString}"=a; a=true;
                                            * }).b` is `true`, confirmed against real Nix) -- see
                                            * compiler.c's AST_ATTRSET case for the `&frame` vs
                                            * `scope` split this depends on. A plain (non-`rec`)
                                            * `{...}`'s dynamic bindings are unaffected -- never
                                            * self-referential either way, same as a plain `{...}`'s
                                            * static bindings. */
            size_t dyn_count;
        } attrset; /* AST_ATTRSET */
        struct {
            Ast *target;
            const char *attr;      /* valid when attr_expr == NULL (static `.name`) */
            size_t attr_len;
            Ast *attr_expr;        /* non-NULL for a dynamic `.${expr}` attribute name --
                                       `attr`/`attr_len` are unused in that case. The name is
                                       resolved (interned) at runtime, not compile time -- see
                                       compiler.c's AST_SELECT case and
                                       src/asm/ops/op_select_dynamic.S. `.${expr} or default`
                                       is supported too -- see
                                       OP_SELECT_DYNAMIC_OR (ir.h) and
                                       src/asm/ops/op_select_dynamic_or.S. */
            Ast *default_; /* `expr.attr or default` -- NULL for a plain select (fatal if
                              * missing/wrong type, see compiler.c's AST_SELECT case). A
                              * multi-segment chain (`a.b.c or default`) shares the SAME
                              * `default_` `Ast*` across every node of the chain (stamped by
                              * parse_select's post-loop walk, parser.c) -- compiler.c's
                              * AST_SELECT case recognizes that sharing (by pointer identity)
                              * and compiles the whole chain as one unit against a single
                              * landing pad, matching real Nix's `ExprSelect::eval`, which
                              * falls through to `default` on ANY segment's failure, not just
                              * this node's own. See compiler.c's AST_SELECT case comment for
                              * the full mechanism. */
        } select; /* AST_SELECT */
        struct {
            Ast *lhs, *rhs;
        } logic; /* AST_AND / AST_OR -- compiled branch-shaped (if/then/else), not as an
                   * eager binop, so the untaken side is never evaluated (see compiler.c). */
        struct {
            Ast *operand;
        } not1; /* AST_NOT */
        struct {
            Ast *cond, *body;
            const char *cond_text; /* raw source span of `cond`, for the failure message */
            size_t cond_text_len;
        } assert1; /* AST_ASSERT */
        struct {
            Ast *attrs;
            Ast *body;
        } with1; /* AST_WITH */
        struct {
            Ast *target;
            const char *attr;      /* valid when attr_expr == NULL (static segment) */
            size_t attr_len;
            Ast *attr_expr;        /* non-NULL for a dynamic `? ${expr}` segment -- same
                                       split as AST_SELECT.attr_expr (see above); resolved
                                       at runtime via OP_HAS_ATTR_DYNAMIC, not interned at
                                       compile time. */
        } has_attr; /* AST_HAS_ATTR: `target ? attr` */
        struct {
            const char *text; /* raw lexeme: e.g. "./foo.nix", or the bare name for a search path --
                                * for an interpolated path (interp_count > 0), this is just the
                                * FIRST chunk (e.g. "./" in "./${foo}") */
            size_t len;
            int is_search; /* 1 for `<name>`-style search-path literals, 0 for ordinary path literals */
            int is_home;   /* 1 if this literal (or its first chunk) started with `~` --
                             * see compiler.c's resolve_path_literal/resolve_path_chunk. Always 0
                             * when is_search is set. */
            AstStringPart *interp_parts; /* NULL/0 for a plain, non-interpolated literal --
                                           * otherwise the parts AFTER `text`, each a literal chunk
                                           * or an interpolated expression, same shape AST_STRING
                                           * already uses (reused directly rather than inventing a
                                           * second one). */
            size_t interp_count;
        } path; /* AST_PATH */
        struct {
            int line;   /* 1-based, matching real Nix's own `__curPos`/`unsafeGetAttrPos` */
            int column; /* 1-based, ditto */
        } curpos; /* AST_CURPOS */
    } as;
};

/* perf: owns its own dedicated, resettable chunk list (ast.c) -- NOT the
 * main never-free arena -- since an Ast tree is pure compile-time
 * scratch, fully dead once ll_compile()/ll_compile_scoped() walks
 * it into an IrProgram. */
Ast *ll_ast_new(AstKind kind);

/* Same as ll_ast_new, plus sets the new node's src_pos (see struct
 * Ast's own comment above) -- used by parser.c's real construction
 * sites, which always know the current token's byte offset.
 *
 * LL_IRDUMP_LINE_TRACKING-gated, not just a runtime no-op: an always-
 * inlined "set one extra field" version costs a measurable instruction
 * regression on hot parser.c call sites, so irdump.c is compiled as its
 * own variant build (see Makefile's IRDUMP_LINE_TRACKING_SRCS) with this
 * macro defined; the real production build never defines it, so
 * production object code is unaffected. When undefined,
 * `ll_ast_new_at(kind, pos)` macro-expands to plain `ll_ast_new
 * (kind)` -- a pure textual substitution, so `pos`'s expression is
 * never even evaluated. */
#ifdef LL_IRDUMP_LINE_TRACKING
static inline Ast *ll_ast_new_at(AstKind kind, uint32_t src_pos) {
    Ast *n = ll_ast_new(kind);
    n->src_pos = src_pos;
    return n;
}
#else
#define ll_ast_new_at(kind, src_pos) ll_ast_new(kind)
#endif

/* Frees every Ast node allocated since the last reset (or process
 * start). Call right after a top-level ll_compile()/
 * ll_compile_scoped() returns -- see ast.c's own header comment for
 * why that's always safe. */
void ll_ast_arena_reset(void);

/* Lazy-thunk-compile experiment (docs/lazy-thunk-compile-proposal.md):
 * redirects every ll_ast_new()/ll_ast_new_at() call made while active
 * into a SEPARATE chunk list that ll_ast_arena_reset() never touches --
 * i.e. permanently retained for the rest of the process, since a
 * deferred lazy-compile stub's Ast subtree must survive past the file's
 * own compile call. Gated internally on ll_lazy_thunk_enabled
 * (compiler.h) -- a no-op when the experiment is off. parser.c's own
 * call sites toggle this around exactly the subtree the experiment
 * defers (an attrset literal binding's value expression). Returns the
 * previous state (nested-safe: a `let`/attrset nested INSIDE an
 * already-redirected value expression must stay redirected for its own
 * nested attrset fields too). */
int ll_ast_enter_retained(void);
void ll_ast_leave_retained(int prev_state);

#endif /* LL_AST_H */
