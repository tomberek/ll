/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "parser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A flat, append-then-finalize vector whose backing store is one SHARED,
 * never-freed-mid-parse buffer reused across EVERY node of this same
 * kind for the whole file, instead of each node paying for its own
 * fresh malloc-from-cap-0. Still produces this project's usual "each Ast
 * node owns its own permanent, exact-size array" output shape (via
 * `_commit`'s exact-size copy-out).
 *
 * Safety argument for why sharing ONE buffer across the whole file is
 * sound: a NESTED use of the exact same vector kind -- e.g. a list
 * literal nested inside another list literal's own element expression,
 * or a lambda-formal's default-value expression that itself contains
 * another lambda pattern -- always runs its own complete push...push...
 * commit cycle and shrinks the shared length back down to where it
 * found it BEFORE the outer use's own next push, simply because that's
 * how a recursive-descent parser's call/return structure works (the
 * inner construct's Ast* is fully built and returned up the call stack
 * before the outer loop resumes). So at every point `_push` or `_commit`
 * actually runs, the buffer's current length is always exactly
 * "whatever the (possibly several, properly nested) callers above me on
 * the C call stack have each already pushed, plus however far EACH ONE
 * has run so far" -- never corrupted by a sibling call that hasn't
 * returned yet. The one discipline this places on every call site:
 * never dereference a stale `T*` captured before a call that might
 * recurse into the SAME macro instance (a `realloc` inside a nested
 * `_push` can move the buffer) -- every call site below only ever reads
 * from the buffer via a fresh `_commit(mark, ...)` call made AFTER all
 * such recursion for that span has already returned, never a cached
 * pointer held across one.
 *
 * NOT applied to BTVec/BindingVec/DynBindingVec (irregularly-nested,
 * tree-shaped, incrementally mutated-and-reopened lifetimes -- see
 * BTVec's own header comment above -- where several of these can be
 * simultaneously "open" in a non-LIFO order, so this same argument does
 * NOT hold; left exactly as before, each with its own fresh `{0}`-
 * started allocation per node). */
#define SCRATCH_VEC(name, T)                                                                                           \
    static T *name##_buf = NULL;                                                                                       \
    static size_t name##_len = 0, name##_cap = 0;                                                                      \
    static size_t name##_mark(void) { return name##_len; }                                                             \
    static void name##_push(T item) {                                                                                  \
        if (name##_len == name##_cap) {                                                                                \
            name##_cap = name##_cap ? name##_cap * 2 : 24;                                                              \
            name##_buf = realloc(name##_buf, name##_cap * sizeof(T));                                                   \
        }                                                                                                               \
        name##_buf[name##_len++] = item;                                                                                \
    }                                                                                                                   \
    /* Copies the span pushed since `mark` into a fresh, exact-size, caller-owned      \
     * array and rewinds the shared buffer back to `mark` -- MUST be called only       \
     * after every nested use of this same macro instance (if any) has already         \
     * committed and returned; see this macro's own header comment above. */           \
    static T *name##_commit(size_t mark, size_t *out_count) {                                                          \
        size_t n = name##_len - mark;                                                                                   \
        name##_len = mark;                                                                                              \
        *out_count = n;                                                                                                 \
        if (n == 0) return NULL;                                                                                        \
        T *out = malloc(n * sizeof(T));                                                                                 \
        memcpy(out, name##_buf + mark, n * sizeof(T));                                                                  \
        return out;                                                                                                     \
    }

SCRATCH_VEC(astvec, Ast *)

typedef struct {
    AstBinding *items;
    size_t len, cap;
} BindingVec;

static void bindingvec_push(BindingVec *v, AstBinding item) {
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 24;
        v->items = realloc(v->items, v->cap * sizeof(AstBinding));
    }
    v->items[v->len++] = item;
}

/* BindingVec (above) doubles its capacity on growth -- correct
 * amortized-O(1) push, but whichever fill fraction it happens to land on
 * when parsing finishes gets handed straight into PERMANENT Ast storage
 * (attrset/let bindings) unless trimmed here first. Only BindingVec
 * (tightly coupled to BTVec's own tree-shaped, non-LIFO lifetime, see
 * BTVec's header comment) and BTVec's own DynBindingVec still need this
 * macro -- the other vector types use SCRATCH_VEC's own `_commit`
 * exact-size copy-out instead. Safe to trim unconditionally before
 * handoff: shrinking realloc never moves/invalidates the still-needed
 * prefix. */
#define VEC_TRIM(v, T)                                                                                               \
    do {                                                                                                             \
        if ((v).len > 0 && (v).len < (v).cap) (v).items = realloc((v).items, (v).len * sizeof(T));                   \
    } while (0)

/* `${expr} = value;` -- a dynamic attrset-literal key. Collected
 * separately from BTVec/BTNode (the static-key tree): the name isn't
 * known until runtime, so it can't participate in that tree's compile-
 * time dup-detection/nested-attrpath merging at all -- see compiler.c's
 * AST_ATTRSET case for how these are applied. Real Nix allows this in a
 * plain `{...}`/`rec {...}` literal but explicitly rejects it in `let`
 * ("dynamic attributes not allowed in let") -- `parse_binding_stmt`'s
 * `dyn_out` parameter is NULL at the one call site (`let`) where that
 * rejection applies. A dynamic segment followed by more attrpath
 * segments (`${k}.a = v;`) or one after a leading static segment
 * (`a.${k} = v;`) is also supported -- see parse_binding_stmt's
 * attrpath-sugar branch and build_dynamic_chain_value, which handle
 * every position uniformly; this vector only ever receives the entries
 * that end up as *this* block's own direct dynamic bindings. */
typedef struct {
    Ast *name_expr;
    Ast *value;
} DynBinding;

typedef struct {
    DynBinding *items;
    size_t len, cap;
} DynBindingVec;

static void dynbindingvec_push(DynBindingVec *v, DynBinding item) {
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 4;
        v->items = realloc(v->items, v->cap * sizeof(DynBinding));
    }
    v->items[v->len++] = item;
}

/* Converts a parse-time DynBindingVec into the plain AstDynBinding array
 * AST_ATTRSET actually stores (ast.h has no reason to know this file's
 * own growable-vector type). */
static AstDynBinding *finalize_dyn_vec(DynBindingVec *vec) {
    if (vec->len == 0) return NULL;
    AstDynBinding *out = malloc(vec->len * sizeof(AstDynBinding));
    for (size_t i = 0; i < vec->len; i++) {
        out[i] = (AstDynBinding){ .name_expr = vec->items[i].name_expr, .value = vec->items[i].value };
    }
    return out;
}

/* Attrpath binding sugar (`a.b.c = val;`, real Nix's `attrpath '=' expr
 * ';'` grammar rule): a *tree* of these is built incrementally while
 * parsing one binds-block (one `{...}`/`rec{...}`/`let`'s immediate
 * binding list), then flattened into ordinary nested AST_ATTRSET Ast
 * nodes only once the whole block is done (finalize_bt_vec below) -- an
 * attrpath statement anywhere in the block can still extend a sibling
 * attrpath statement's nested attrset parsed earlier in the same block
 * (`a.b = 1; a.c = 2;` => `a = { b = 1; c = 2; };`, order-independent),
 * so nothing can be finalized to a flat Ast until the whole block is
 * known.
 *
 * Each node is either a LEAF (name has a definitive value: an ordinary
 * `name = expr;`, or an `inherit`ed name) or a BRANCH (name was only ever
 * used as a non-final attrpath segment, so it implicitly denotes a nested
 * attrset -- never both. This tree only ever holds STATIC segments
 * (`AttrSeg.is_dynamic == 0`) -- a dynamic segment anywhere in an
 * attrpath (`a.${k} = v;`, `${k}.b = v;`, `a.${k1}.${k2} = v;`) is
 * handled by splitting the chain at the first dynamic segment:
 * everything before it still flows through this static tree as normal;
 * everything from the first dynamic segment onward becomes a synthetic
 * nested plain-attrset literal with a `${...} = ...;` dynamic binding as
 * that static prefix's leaf value -- see parse_binding_stmt's
 * attrpath-sugar branch below for exactly where that split happens. */
typedef struct {
    int is_dynamic;
    Ast *name_expr;      /* valid when is_dynamic */
    const char *attr;    /* valid when !is_dynamic */
    size_t attr_len;
} AttrSeg;

typedef struct BTNode BTNode;
/* `dyn` -- dynamic (`${expr} = value;`) bindings absorbed into THIS
 * level of the tree by `reopen_attrset_into` (below), carried alongside
 * the ordinary static `items`/`len`/`cap` since a BTNode has no static
 * name to merge them under. Empty/zero for the overwhelming majority of
 * BTVecs, which never go through a merge involving a dynamic-keyed
 * literal at all.
 *
 * `index`/`index_cap` (maintained by `btvec_push`, consulted by
 * `btvec_find`) is an open-addressing hash index parallel to `items`,
 * turning the dup-attr check inside `insert_attrpath` into O(1)
 * amortized instead of a linear scan per binding statement. */
typedef struct { BTNode *items; size_t len, cap; DynBindingVec dyn; size_t *index; size_t index_cap; } BTVec;

#define BTVEC_EMPTY_SLOT SIZE_MAX

/* Folds a BTVec's own absorbed-via-merge dynamic bindings (see
 * reopen_attrset_into's comment) into this BLOCK's top-level explicit
 * `${expr} = value;` DynBindingVec, so one `finalize_dyn_vec` call at
 * each `{...}`/`rec {...}` call site below picks up both kinds. */
static void merge_bt_dyn_into(BTVec *bt, DynBindingVec *dyn_out) {
    for (size_t i = 0; i < bt->dyn.len; i++) dynbindingvec_push(dyn_out, bt->dyn.items[i]);
}

struct BTNode {
    const char *name;
    size_t name_len;
    int is_leaf;
    Ast *leaf_value;   /* valid iff is_leaf */
    int is_inherit;    /* valid iff is_leaf */
    int is_hidden;     /* valid iff is_leaf -- see AstBinding.is_hidden (ast.h) */
    int rec;           /* valid iff !is_leaf: whether the attrset this branch finalizes
                         * into should be `rec`. Always 0 for a branch created purely by
                         * attrpath-sugar descent (`a.b = 1;` makes an ordinary, non-rec
                         * nested `a`). Only set to 1 when a branch was instead "reopened"
                         * from a pre-existing literal `rec {...}` value below (the
                         * regrettable-merge path) -- it then keeps that literal's original
                         * rec-ness, per real Nix's own documented quirk (NixOS/nix#9020:
                         * "any rec marker on [the earlier value] will apply to the
                         * attributes in [the later one]"). */
    BTVec children;    /* valid iff !is_leaf */
};

static uint64_t btvec_hash_name(const char *name, size_t len) {
    uint64_t h = 0;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)(unsigned char)name[i];
        h *= 0x517cc1b727220a95ULL;
    }
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebULL;
    h ^= h >> 31;
    return h;
}

/* Rebuilds `v->index` from scratch against `v->items[0..v->len)` (the
 * item about to be pushed is NOT yet in `items` when this is called from
 * `btvec_push` -- see there). */
static void btvec_index_rehash(BTVec *v, size_t new_cap) {
    size_t *new_table = malloc(new_cap * sizeof(size_t));
    for (size_t i = 0; i < new_cap; i++) new_table[i] = BTVEC_EMPTY_SLOT;
    for (size_t i = 0; i < v->len; i++) {
        size_t slot = btvec_hash_name(v->items[i].name, v->items[i].name_len) & (new_cap - 1);
        while (new_table[slot] != BTVEC_EMPTY_SLOT) slot = (slot + 1) & (new_cap - 1);
        new_table[slot] = i;
    }
    free(v->index);
    v->index = new_table;
    v->index_cap = new_cap;
}

static void btvec_push(BTVec *v, BTNode item) {
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 24;
        v->items = realloc(v->items, v->cap * sizeof(BTNode));
    }
    /* Grow/rehash the index BEFORE appending -- rehash only ever sees
     * `items[0..len)` (the not-yet-pushed new item isn't in there yet),
     * so the explicit insert below never collides with work rehash just
     * did. 75% load factor, matching symtab.c/derivation.c's tables. */
    if (v->index_cap == 0) btvec_index_rehash(v, 16);
    else if ((v->len + 1) * 4 > v->index_cap * 3) btvec_index_rehash(v, v->index_cap * 2);
    v->items[v->len] = item;
    size_t slot = btvec_hash_name(item.name, item.name_len) & (v->index_cap - 1);
    while (v->index[slot] != BTVEC_EMPTY_SLOT) slot = (slot + 1) & (v->index_cap - 1);
    v->index[slot] = v->len;
    v->len++;
}

static BTNode *btvec_find(BTVec *v, const char *name, size_t name_len) {
    if (v->index_cap == 0) return NULL;
    size_t slot = btvec_hash_name(name, name_len) & (v->index_cap - 1);
    for (;;) {
        size_t idx = v->index[slot];
        if (idx == BTVEC_EMPTY_SLOT) return NULL;
        if (v->items[idx].name_len == name_len && memcmp(v->items[idx].name, name, name_len) == 0) {
            return &v->items[idx];
        }
        slot = (slot + 1) & (v->index_cap - 1);
    }
}

static void fatal_dup_attr(const char *name, size_t name_len) {
    fprintf(stderr, "ll: attribute '%.*s' already defined, aborting\n", (int)name_len, name);
    exit(1);
}

static void insert_attrpath(BTVec *vec, AttrSeg *segs, size_t seg_count, size_t idx,
                             Ast *leaf_value, int is_inherit);

/* Re-opens a pre-existing, already-finalized AST_ATTRSET literal (`attrs`)
 * as a mutable BTVec of leaf nodes, so a later attrpath statement in the
 * same binds-block can still extend or merge into it -- the mechanism
 * behind both the plain "same name, both attrset-literal values" dup-key
 * merge (`inner = {...}; inner = {...};`) and the "regrettable" attrpath-
 * into-a-literal merge (`a = rec {...}; a.c = ...;`, NixOS/nix#9020) via
 * the exact same code path real Nix's addAttr uses for both. */
static void reopen_attrset_into(BTVec *dest, Ast *attrs) {
    /* `a."${k}" = 1; a."${k2}" = 2;` / `a.b = 1; a."${k}" = 2;`-style
     * regrettable merges (NixOS/nix#9020) of a literal that itself had a
     * dynamic key: carried into `dest->dyn` (BTVec's own comment) instead
     * of being dropped or fataling -- `finalize_bt_node`/the top-level
     * block parser (below) re-attach it to whichever AST_ATTRSET this
     * BTVec ultimately finalizes into, same as this project's own direct
     * `${k} = v;` dynamic bindings already were. This project doesn't
     * attempt to replicate real Nix's "two dynamic keys that happen to
     * collide at runtime" dup-attr check, same OP_INSERT_DYNAMIC_ATTR
     * behavior as every other dynamic binding already has. */
    for (size_t i = 0; i < attrs->as.attrset.dyn_count; i++) {
        AstDynBinding *db = &attrs->as.attrset.dyn_bindings[i];
        dynbindingvec_push(&dest->dyn, (DynBinding){ .name_expr = db->name_expr, .value = db->value });
    }
    for (size_t i = 0; i < attrs->as.attrset.count; i++) {
        AstBinding *b = &attrs->as.attrset.bindings[i];
        btvec_push(dest, (BTNode){
            .name = b->name, .name_len = b->name_len,
            .is_leaf = 1, .leaf_value = b->value, .is_inherit = b->is_inherit, .is_hidden = b->is_hidden });
    }
}

static void insert_attrpath(BTVec *vec, AttrSeg *segs, size_t seg_count, size_t idx,
                             Ast *leaf_value, int is_inherit) {
    AttrSeg seg = segs[idx];
    BTNode *existing = btvec_find(vec, seg.attr, seg.attr_len);
    int is_last = (idx == seg_count - 1);
    if (is_last) {
        if (!existing) {
            btvec_push(vec, (BTNode){
                .name = seg.attr, .name_len = seg.attr_len,
                .is_leaf = 1, .leaf_value = leaf_value, .is_inherit = is_inherit });
            return;
        }
        /* Collision at the final segment: only mergeable if BOTH the
         * existing value and the new value are themselves literal
         * attrsets (real Nix's exact condition -- see addAttr's
         * `jAttrs && ae` check) -- anything else is a hard dup-attr
         * error, matching real Nix. */
        Ast *existing_attrs_val = existing->is_leaf ? existing->leaf_value : NULL;
        int existing_is_attrs_literal = existing->is_leaf && existing_attrs_val &&
                                         existing_attrs_val->kind == AST_ATTRSET;
        int new_is_attrs_literal = !is_inherit && leaf_value && leaf_value->kind == AST_ATTRSET;
        if (!existing->is_leaf && new_is_attrs_literal) {
            /* existing is already a branch (from earlier attrpath sugar) -- merge the
             * new literal attrset's own bindings into it. */
            reopen_attrset_into(&existing->children, leaf_value);
            return;
        }
        if (existing_is_attrs_literal && new_is_attrs_literal) {
            BTVec merged = { 0 };
            reopen_attrset_into(&merged, existing_attrs_val);
            existing->is_leaf = 0;
            existing->rec = existing_attrs_val->as.attrset.is_rec;
            existing->children = merged;
            reopen_attrset_into(&existing->children, leaf_value);
            return;
        }
        fatal_dup_attr(seg.attr, seg.attr_len);
    }
    /* Not the last segment: this name must denote (or become) a branch. */
    if (!existing) {
        BTNode node = { .name = seg.attr, .name_len = seg.attr_len, .is_leaf = 0, .rec = 0 };
        btvec_push(vec, node);
        existing = &vec->items[vec->len - 1];
        insert_attrpath(&existing->children, segs, seg_count, idx + 1, leaf_value, is_inherit);
        return;
    }
    if (existing->is_leaf) {
        /* Only mergeable if the existing leaf's value is itself a literal
         * attrset (the "regrettable" merge path -- real Nix's addAttr
         * dynamic_casts the existing value to ExprAttrs here too). */
        if (!existing->leaf_value || existing->leaf_value->kind != AST_ATTRSET) {
            fatal_dup_attr(seg.attr, seg.attr_len);
        }
        Ast *reopened = existing->leaf_value;
        existing->is_leaf = 0;
        existing->rec = reopened->as.attrset.is_rec;
        existing->children = (BTVec){ 0 };
        reopen_attrset_into(&existing->children, reopened);
    }
    insert_attrpath(&existing->children, segs, seg_count, idx + 1, leaf_value, is_inherit);
}

static AstBinding finalize_bt_node(BTNode *node) {
    if (node->is_leaf) {
        return (AstBinding){
            .name = node->name, .name_len = node->name_len,
            .value = node->leaf_value, .is_inherit = node->is_inherit, .is_hidden = node->is_hidden };
    }
    BindingVec out = { 0 };
    for (size_t i = 0; i < node->children.len; i++) {
        bindingvec_push(&out, finalize_bt_node(&node->children.items[i]));
    }
    VEC_TRIM(out, AstBinding);
    Ast *attrs = ll_ast_new(AST_ATTRSET);
    attrs->as.attrset.bindings = out.items;
    attrs->as.attrset.count = out.len;
    attrs->as.attrset.is_rec = node->rec;
    /* Re-attach whatever dynamic bindings this branch absorbed via
     * reopen_attrset_into (see that function's own comment) -- a nested
     * synthetic attrset needs its own dyn_bindings just like a top-level
     * one does. */
    attrs->as.attrset.dyn_bindings = finalize_dyn_vec(&node->children.dyn);
    attrs->as.attrset.dyn_count = node->children.dyn.len;
    /* Everything this branch's subtree needs has now been copied out
     * (leaf values are Ast* already living in the arena; finalize_dyn_vec
     * copies its own exact-size array) -- nothing retains node->children's
     * own backing arrays past this point, so free all three of them,
     * including `.index` (the open-addressing hash table backing
     * btvec_find), a SEPARATE malloc'd array from `.items`. */
    free(node->children.items);
    free(node->children.dyn.items);
    free(node->children.index);
    return (AstBinding){ .name = node->name, .name_len = node->name_len, .value = attrs, .is_inherit = 0 };
}

static void finalize_bt_vec(BTVec *vec, BindingVec *out) {
    for (size_t i = 0; i < vec->len; i++) {
        bindingvec_push(out, finalize_bt_node(&vec->items[i]));
    }
    free(vec->items);
    free(vec->index);
}

SCRATCH_VEC(partvec, AstStringPart)

/* A growable array of AttrSeg -- used by parse_binding_stmt to collect an
 * attrpath's segments (`a.b.c`/`a.${k}.c`) before handing them to
 * insert_attrpath (the static prefix) and/or build_dynamic_chain_value
 * (everything from the first dynamic segment onward). */
SCRATCH_VEC(attrsegvec, AttrSeg)

SCRATCH_VEC(formalvec, AstFormal)

/* Raw piece of a '' ... '' literal as collected straight off the lexer,
 * before indentation stripping -- same shape as AstStringPart plus the
 * per-literal-chunk `has_indentation` flag (see lexer.h's TOK_ISTR_PART
 * comment). Kept separate from AstStringPart because that flag has no
 * meaning once stripping is done. */
typedef struct {
    int is_expr;
    const char *text;
    size_t text_len;
    int has_indentation;
    Ast *expr;
} RawIndPart;

SCRATCH_VEC(rawind, RawIndPart)

/* Ported directly from real Nix's ParserState::stripIndentation
 * (~/nix's src/libexpr/include/nix/expr/parser-state.hh). Two
 * passes over the raw parts: (1) find the minimum leading-space count
 * across every content line (a line ended by an escape or an
 * interpolation, not just a raw newline, still ends the "start of line"
 * run -- matching real Nix's `!str->hasIndentation` / expr handling); (2)
 * strip exactly that many leading spaces from every content line, and (for
 * a literal's very last raw part only) drop a final line that is entirely
 * spaces. */
static AstStringPart *strip_indentation(RawIndPart *raw, size_t n, size_t *out_count) {
    if (n == 0) {
        AstStringPart *parts = malloc(sizeof(AstStringPart));
        parts[0] = (AstStringPart){ .is_expr = 0, .text = "", .text_len = 0 };
        *out_count = 1;
        return parts;
    }

    int at_start = 1;
    size_t min_indent = 1000000;
    size_t cur_indent = 0;
    for (size_t idx = 0; idx < n; idx++) {
        RawIndPart *p = &raw[idx];
        if (p->is_expr || !p->has_indentation) {
            if (at_start) {
                at_start = 0;
                if (cur_indent < min_indent) min_indent = cur_indent;
            }
            continue;
        }
        for (size_t j = 0; j < p->text_len; j++) {
            char ch = p->text[j];
            if (at_start) {
                if (ch == ' ') cur_indent++;
                else if (ch == '\n') cur_indent = 0;
                else {
                    at_start = 0;
                    if (cur_indent < min_indent) min_indent = cur_indent;
                }
            } else if (ch == '\n') {
                at_start = 1;
                cur_indent = 0;
            }
        }
    }

    AstStringPart *out = malloc(n * sizeof(AstStringPart)); /* upper bound: at most one output per input part */
    size_t result_count = 0;
    at_start = 1;
    size_t cur_dropped = 0;
    for (size_t idx = 0; idx < n; idx++) {
        RawIndPart *p = &raw[idx];
        if (p->is_expr) {
            at_start = 0;
            cur_dropped = 0;
            out[result_count++] = (AstStringPart){ .is_expr = 1, .expr = p->expr };
            continue;
        }

        char *s2 = malloc(p->text_len ? p->text_len : 1);
        size_t s2len = 0;
        for (size_t j = 0; j < p->text_len; j++) {
            char ch = p->text[j];
            if (at_start) {
                if (ch == ' ') {
                    if (cur_dropped++ >= min_indent) s2[s2len++] = ch;
                } else if (ch == '\n') {
                    cur_dropped = 0;
                    s2[s2len++] = ch;
                } else {
                    at_start = 0;
                    cur_dropped = 0;
                    s2[s2len++] = ch;
                }
            } else {
                s2[s2len++] = ch;
                if (ch == '\n') at_start = 1;
            }
        }

        /* Drop a final line that's empty-or-spaces-only, but only for the
         * literal's overall last raw part (matches upstream's `n == 1`
         * check, where its `n` is "parts remaining including this one",
         * i.e. true exactly on the last loop iteration). */
        if (idx == n - 1) {
            size_t last_nl = (size_t)-1;
            for (size_t k = 0; k < s2len; k++) if (s2[k] == '\n') last_nl = k;
            if (last_nl != (size_t)-1) {
                int all_spaces_after = 1;
                for (size_t k = last_nl + 1; k < s2len; k++) {
                    if (s2[k] != ' ') { all_spaces_after = 0; break; }
                }
                if (all_spaces_after) s2len = last_nl + 1;
            }
        }

        if (s2len > 0) {
            out[result_count++] = (AstStringPart){ .is_expr = 0, .text = s2, .text_len = s2len };
        } else {
            free(s2);
        }
    }

    if (result_count == 0) {
        out[0] = (AstStringPart){ .is_expr = 0, .text = "", .text_len = 0 };
        result_count = 1;
    }
    *out_count = result_count;
    return out;
}

typedef struct {
    Lexer *lx;  /* A true pull-based lexer (lexer.h) -- no backing Token array at
                 * all. `tok`/`next` below are this parser's own 2-slot lookahead
                 * buffer, by VALUE, filled by calling lexer_next(lx) -- cur()/
                 * peek2() are plain field reads, never a lexer.c call; only
                 * advance() ever calls lexer_next(). */
    Token tok;  /* cur() */
    Token next; /* peek2() */
    const char *src; /* The buffer every Token.pos/.start points into -- see
                       * curpos_line_col below, parser.h's own ll_parse comment. */
    const char *curpos_base; /* Marker-reparse addition: the TRUE file-start pointer for
                       * `__curPos`'s line/column (curpos_line_col below) -- equal to `src`
                       * for an ordinary ll_parse() call (whose Lexer starts right at the
                       * real file start, so the two coincide), but ll_parse_subexpr's
                       * Lexer starts partway into the real file (at some byte offset into the
                       * SAME underlying buffer) while `__curPos`'s line/col must still be
                       * reported relative to the real file start, not the sub-parse's own
                       * token-offset-0 point -- see that function's own comment. */
} Parser;

/* 1-based line/column of `at` (a pointer into `src`, e.g. a
 * `Token.pos`) -- counts newlines from the start of the buffer up to
 * `at`, same convention real Nix's own `Pos` uses. Only ever called for
 * the rare `__curPos` occurrence (not once per token), so an O(file
 * size) scan here is a non-issue -- real Nix itself only computes a
 * `Pos`'s line/column lazily, on demand, for exactly the same reason
 * (`Pos::LazyLine`/`actualPos`, `~/nix/src/libutil/position.cc`). */
static void curpos_line_col(const char *src, const char *at, int *out_line, int *out_col) {
    int line = 1, col = 1;
    for (const char *c = src; c < at; c++) {
        if (*c == '\n') { line++; col = 1; }
        else col++;
    }
    *out_line = line;
    *out_col = col;
}

/* Every Token carries a raw [start,end) source-byte-offset span (see
 * lexer.h) -- these two tiny helpers recover the "pointer into src, plus
 * length" shape almost every call site below still wants, for the
 * (overwhelming majority of) token kinds whose span IS its final text
 * verbatim (identifiers, keywords, path/URI/search-path literals --
 * never TOK_STR_PART/TOK_ISTR_PART/TOK_ISTR_ESC, which need a real
 * decode step instead, see ll_decode_dquote_span/ll_decode_istr_esc's
 * own call sites below). */
static const char *tok_text(Parser *p, Token t) { return p->src + t.start; }
static size_t tok_len(Token t) { return (size_t)(t.end - t.start); }

static Token cur(Parser *p) {
    return p->tok;
}
static Token peek2(Parser *p) {
    return p->next;
}
/* `is_formals_pattern`'s `{...}`-vs-formals disambiguation needs a THIRD
 * token of lookahead beyond `tok`/`next` -- probed by saving the lexer's
 * full resumable state, calling lexer_next() once to see what comes
 * after `next`, then rewinding immediately. Only valid right after
 * peek2(): this is only ever called with the lexer already sitting
 * exactly one token past `next`, which lexer_save() below captures
 * regardless. */
static TokKind peek3(Parser *p) {
    LexerSave save = lexer_save(p->lx);
    TokKind k = lexer_next(p->lx).kind;
    lexer_restore(p->lx, save);
    return k;
}
static void advance(Parser *p) {
    p->tok = p->next;
    p->next = lexer_next(p->lx);
}
static Token expect(Parser *p, TokKind kind, const char *what) {
    if (cur(p).kind != kind) {
        fprintf(stderr, "ll: parse error: expected %s\n", what);
        exit(1);
    }
    Token t = cur(p);
    advance(p);
    return t;
}

/* `or` is lexed as its own keyword token (TOK_OR, for the `.attr or
 * default` select-with-default form -- see parse_select), but real Nix's
 * grammar still accepts it as an ORDINARY IDENTIFIER in any position that
 * can't be confused with that form -- a binding/formal name, or a bare
 * variable reference (deprecated/"cursed" there with a warning, per
 * NixOS/nix#11121, but still accepted, not rejected). `starts_atom`/
 * `parse_atom` below treat a bare TOK_OR exactly like TOK_IDENT for this
 * reason. */
static int starts_atom(TokKind k) {
    return k == TOK_INT || k == TOK_FLOAT || k == TOK_IDENT || k == TOK_TRUE || k == TOK_FALSE || k == TOK_NULL ||
           k == TOK_LPAREN || k == TOK_STR_START || k == TOK_ISTR_START || k == TOK_LBRACKET ||
           k == TOK_LBRACE || k == TOK_REC || k == TOK_PATH || k == TOK_SEARCH_PATH || k == TOK_OR ||
           k == TOK_URI;
}

static Ast *parse_expr(Parser *p);
static Ast *parse_dquote_string(Parser *p);

/* List elements and application arguments are parsed at this same
 * "select" precedence (atom plus postfix `.attr` chains), not full
 * `expr` -- matching real Nix, where `[ f x ]` is a 2-element list and
 * putting a function call in a list needs parens: `[ (f x) ]`. */
static Ast *parse_select(Parser *p);

/* Parses one binding-list statement (an ordinary `name = value;`, or an
 * `inherit x y;` / `inherit (expr) x y;` form) and appends the resulting
 * binding(s) to `out`. Shared by `let ... in`, `rec { ... }`, and plain
 * `{ ... }` -- all three use the same binding-list grammar (see ast.h's
 * note on AstBinding.is_inherit for why the desugared value differs by
 * form but `is_inherit` is attached regardless). */

/* Parses one attrpath segment -- shared by `inherit`'s attribute-name
 * list, attrpath binding sugar (`a."b c".${k}.d = val;`), `.`-select
 * chains, and `?`-chains. Needed for names that collide with a reserved
 * keyword and so can't be written bare (e.g. nixpkgs's own
 * `lib/trivial.nix`: `"or" = x: y: x || y;`). A dynamic segment (bare
 * `${expr}` or an interpolated quoted string) is likewise a real,
 * supported case in every one of these contexts EXCEPT `inherit` --
 * that one rejects it unconditionally, confirmed in cppnix's own
 * grammar (`attrs: attrs string_attr`'s visitor throws "dynamic
 * attributes not allowed in inherit" for the `Expr*`/dynamic case
 * specifically, not the plain-string case) -- see that call site's own
 * dedicated rejection check, not here, since a static quoted-string
 * segment must still be accepted for `inherit`. */
static AttrSeg parse_attr_segment(Parser *p, const char *what) {
    if (cur(p).kind == TOK_DOLLAR_LBRACE) {
        advance(p);
        Ast *name_expr = parse_expr(p);
        expect(p, TOK_RBRACE, "'}' closing a dynamic attribute name");
        /* A bare `${"literal"}` (no interpolation inside the string
         * literal itself -- `${"a" + "b"}` or `${foo}` still count as
         * genuinely dynamic) is a compile-time-constant name, same as
         * the quoted-string branch below already folds. Real Nix does
         * this too, generally: `~/nix/src/libexpr/include/nix/expr/
         * parser-state.hh`'s `ToBeStringyExpr::visit`, "consistently
         * handle simple StringExpr ${"string"} as non-dynamic
         * attributes" (NixOS/nix#14642). `let ${"a"} = 1; in a` == `1`,
         * but `let ${"a"+""} = 1; in a` still fails "dynamic attributes
         * not allowed in let" since the inner expression is genuinely
         * dynamic. */
        if (name_expr->kind == AST_STRING) {
            if (name_expr->as.string.count == 0) {
                return (AttrSeg){ .is_dynamic = 0, .attr = "", .attr_len = 0 };
            }
            if (name_expr->as.string.count == 1 && !name_expr->as.string.parts[0].is_expr) {
                return (AttrSeg){ .is_dynamic = 0, .attr = name_expr->as.string.parts[0].text,
                                   .attr_len = name_expr->as.string.parts[0].text_len };
            }
        }
        return (AttrSeg){ .is_dynamic = 1, .name_expr = name_expr };
    }
    if (cur(p).kind != TOK_STR_START) {
        /* `or`/`null`/`true`/`false` are also valid as a bare (unquoted)
         * binding/attribute name -- see starts_atom's own comment above
         * for the `or` rationale; `{ null = 1; }.null` == `1`, same for
         * `true`/`false`. Real Nix's own structural keywords (`let`/
         * `in`/`if`/`then`/`else`/`with`/`assert`/`inherit`/`rec`) stay
         * rejected here -- only `or`/`null`/`true`/`false` are real
         * Nix's own bare-identifier-shaped literal tokens, lexed as such
         * but still accepted as plain names in this one grammar
         * position. */
        if (cur(p).kind == TOK_OR || cur(p).kind == TOK_NULL || cur(p).kind == TOK_TRUE || cur(p).kind == TOK_FALSE) {
            Token t = cur(p);
            advance(p);
            return (AttrSeg){ .is_dynamic = 0, .attr = tok_text(p, t), .attr_len = tok_len(t) };
        }
        Token t = expect(p, TOK_IDENT, what);
        return (AttrSeg){ .is_dynamic = 0, .attr = tok_text(p, t), .attr_len = tok_len(t) };
    }
    /* Quoted-string segment -- reuses parse_dquote_string's own chunk-
     * collecting loop (same TOK_STR_PART/TOK_DOLLAR_LBRACE/TOK_STR_END
     * grammar as an ordinary string literal), then classifies the
     * result: zero parts (`""`) or exactly one non-interpolated literal
     * part is a compile-time-constant name (static); anything else
     * (an interpolation chunk present) is genuinely dynamic. */
    Ast *str = parse_dquote_string(p);
    if (str->as.string.count == 0) {
        return (AttrSeg){ .is_dynamic = 0, .attr = "", .attr_len = 0 };
    }
    if (str->as.string.count == 1 && !str->as.string.parts[0].is_expr) {
        return (AttrSeg){ .is_dynamic = 0, .attr = str->as.string.parts[0].text,
                           .attr_len = str->as.string.parts[0].text_len };
    }
    return (AttrSeg){ .is_dynamic = 1, .name_expr = str };
}

static void set_has_attr_segment(Ast *n, AttrSeg seg) {
    if (seg.is_dynamic) {
        n->as.has_attr.attr_expr = seg.name_expr;
        n->as.has_attr.attr = NULL;
        n->as.has_attr.attr_len = 0;
    } else {
        n->as.has_attr.attr_expr = NULL;
        n->as.has_attr.attr = seg.attr;
        n->as.has_attr.attr_len = seg.attr_len;
    }
}

static void set_select_segment(Ast *n, AttrSeg seg) {
    if (seg.is_dynamic) {
        n->as.select.attr_expr = seg.name_expr;
        n->as.select.attr = NULL;
        n->as.select.attr_len = 0;
    } else {
        n->as.select.attr_expr = NULL;
        n->as.select.attr = seg.attr;
        n->as.select.attr_len = seg.attr_len;
    }
}

/* Builds the value for an attrpath-sugar binding's segments
 * `segs[start..start+count)`, given the final `value` expression --
 * used for the portion of an attrpath STARTING AT the first dynamic
 * segment (`a.${k}.b = v;`, `${k}.b = v;`, `a.${k1}.${k2} = v;`).
 * `count == 0` is the base case (the chain is exhausted; just return
 * `value` itself). Otherwise wraps the recursively-built inner value in
 * one more layer: a plain (non-`rec`) one-binding attrset literal, using
 * either an ordinary static binding (`{ segs[start] = inner; }`) or the
 * dynamic-binding machinery (`{ ${segs[start]} = inner; }`) depending on
 * that segment's own kind -- so a chain with multiple dynamic segments
 * (`a.${k1}.${k2} = v;`) composes correctly, each nested dynamic binding
 * getting its own OP_INSERT_DYNAMIC_ATTR at runtime. */
static Ast *build_dynamic_chain_value(AttrSeg *segs, size_t start, size_t count, Ast *value) {
    if (count == 0) return value;
    AttrSeg seg = segs[start];
    Ast *inner = build_dynamic_chain_value(segs, start + 1, count - 1, value);
    Ast *attrs = ll_ast_new(AST_ATTRSET);
    attrs->as.attrset.is_rec = 0;
    if (seg.is_dynamic) {
        attrs->as.attrset.bindings = NULL;
        attrs->as.attrset.count = 0;
        AstDynBinding *dyn = malloc(sizeof(AstDynBinding));
        dyn[0] = (AstDynBinding){ .name_expr = seg.name_expr, .value = inner };
        attrs->as.attrset.dyn_bindings = dyn;
        attrs->as.attrset.dyn_count = 1;
    } else {
        AstBinding *b = malloc(sizeof(AstBinding));
        b[0] = (AstBinding){ .name = seg.attr, .name_len = seg.attr_len, .value = inner, .is_inherit = 0 };
        attrs->as.attrset.bindings = b;
        attrs->as.attrset.count = 1;
        attrs->as.attrset.dyn_bindings = NULL;
        attrs->as.attrset.dyn_count = 0;
    }
    return attrs;
}

/* `dyn_out`: NULL rejects a dynamic `${...}` key outright ("dynamic
 * attributes not allowed in let", matching real Nix's own explicit
 * rejection there); non-NULL (plain `{...}`/`rec {...}`) collects it
 * instead of fataling. See DynBindingVec's own comment above. */
/* Every synthetic hidden binding `inherit (expr) a b c;` generates
 * (below) gets a name built from this counter, prefixed with a byte
 * ('\x01') the lexer can never produce inside an identifier/string-attr
 * token -- guarantees it can never collide with a real, source-written
 * attribute name, so it's safe to `btvec_push` directly (bypassing
 * insert_attrpath's dup-attr/merge checks entirely, which exist only
 * for names a *user* could write). */
static int g_inherit_hidden_counter = 0;

/* Lazy-thunk-plus-bytecode-cache reconciliation (see compiler.c's own
 * LazyThunkDescriptor comment): when a deferred attrset-field value gets
 * cached as an unresolved stub, bytecode.c needs a real source byte
 * offset to reparse `value` from in a FRESH process (Ast->src_pos is
 * normally only populated under LL_IRDUMP_LINE_TRACKING -- a debug-
 * only build variant, never set in production -- so it's always 0 here
 * otherwise). Declared `extern` rather than pulling in the whole of
 * compiler.h, to avoid adding an include-order dependency to this file
 * for one flag. */
extern int ll_lazy_thunk_enabled;

/* Lazy-thunk-compile experiment, ported from asnix (see ast.h's own
 * comment on ll_ast_enter_retained): wraps the real
 * parse_binding_stmt_inner below so every Ast node it builds for ANY
 * binding -- attrset-literal OR `let` -- is redirected into the
 * permanently-retained arena instead of the normal per-file resettable
 * one. Unconditional on block kind (not gated on `dyn_out != NULL`,
 * which asnix's own M196 fix found was wrong: `let a.b = v;`-style
 * attrpath sugar synthesizes an AST_ATTRSET node regardless of block
 * kind, and that synthetic attrset's own fields are just as eligible
 * for emit_attrset_field_value's deferral as any hand-written literal's
 * -- a real neovim.outPath use-after-free on asnix before this fix). */
static void parse_binding_stmt_inner(Parser *p, BTVec *out, DynBindingVec *dyn_out);

static void parse_binding_stmt(Parser *p, BTVec *out, DynBindingVec *dyn_out) {
    int prev = ll_ast_enter_retained();
    parse_binding_stmt_inner(p, out, dyn_out);
    ll_ast_leave_retained(prev);
}

static void parse_binding_stmt_inner(Parser *p, BTVec *out, DynBindingVec *dyn_out) {
    if (cur(p).kind == TOK_INHERIT) {
        advance(p);
        Ast *src_expr = NULL;
        if (cur(p).kind == TOK_LPAREN) {
            advance(p);
            src_expr = parse_expr(p);
            expect(p, TOK_RPAREN, "')'");
        }
        /* Collect every inherited name first (real Nix's own grammar,
         * `attrs: attrs attr | attrs string_attr`, already requires this
         * -- one `inherit (expr) ...;` clause's names are all read before
         * anything is done with them) so the `src_expr != NULL` case
         * below can decide, from the final count, whether to share one
         * evaluation of `expr` across every name. */
        size_t names_mark = attrsegvec_mark();
        /* `or`/`null`/`true`/`false` are also valid bare inherited names
         * (parse_attr_segment's own comment) -- this loop's own
         * continuation check must recognize the same token set it hands
         * off to, or an inherit clause starting with one of them (e.g.
         * `inherit (x) null true;`) parses zero names and chokes on the
         * first one expecting ';' instead. */
        while (cur(p).kind == TOK_IDENT || cur(p).kind == TOK_STR_START || cur(p).kind == TOK_DOLLAR_LBRACE ||
               cur(p).kind == TOK_OR || cur(p).kind == TOK_NULL || cur(p).kind == TOK_TRUE || cur(p).kind == TOK_FALSE) {
            AttrSeg name = parse_attr_segment(p, "an inherited name");
            /* real Nix's grammar lets a bare `${...}`/interpolated
             * string reach this position syntactically (`attrs: attrs
             * attr | attrs string_attr`), but rejects it unconditionally
             * once parsed -- confirmed in cppnix's own source (the
             * `string_attr` visitor throws "dynamic attributes not
             * allowed in inherit" specifically for the dynamic/`Expr*`
             * case, not the plain-string case, so `inherit (x) "or";`
             * still works -- see parse_attr_segment's own comment). */
            if (name.is_dynamic) {
                fprintf(stderr, "ll: parse error: dynamic attributes not allowed in inherit\n");
                exit(1);
            }
            attrsegvec_push(name);
        }
        /* Commit now, right as collection ends -- nothing between here
         * and this clause's own final `free` below recurses into the
         * parser (btvec_push/ll_ast_new/insert_attrpath are pure
         * tree-manipulation, see SCRATCH_VEC's own safety argument). */
        size_t names_count;
        AttrSeg *names_items = attrsegvec_commit(names_mark, &names_count);
        /* `inherit (expr) a b;` must evaluate `expr` exactly ONCE, shared
         * by every inherited name, matching real Nix's own parser.y/
         * nixexpr.cc (every `inherit (expr) ...;` clause gets exactly one
         * shared `inheritFromExprs` thunk). Re-embedding the SAME
         * `src_expr` Ast* pointer directly as each name's own
         * `AST_SELECT.target` would be wrong whenever `expr` is a genuine
         * expression with its own side effect or allocation cost, since
         * compiler.c gives every binding (including each of these
         * selects) its own independent `OP_MK_THUNK`, so `target` would
         * really be *recompiled and re-run* once per inherited name, not
         * shared.
         *
         * Fixed by introducing one synthetic hidden binding (named via
         * `g_inherit_hidden_counter` above, `is_hidden` so neither
         * compiler.c's `AST_ATTRSET` case nor main.c's printer ever
         * treats it as a real attribute) holding `src_expr` itself, and
         * having every name's `AST_SELECT.target` be an `AST_VAR`
         * referencing that ONE binding instead -- it then goes through
         * the exact same frame/env-slot machinery (`OP_MK_THUNK` once,
         * `OP_FRAME_GET`/`OP_VAR_REF` to re-read the already-forced
         * result from then on) every ordinary shared `let`/`rec {}`
         * binding already uses to memoize itself, with zero new runtime
         * machinery needed. */
        Ast *hidden_var = NULL;
        if (src_expr && names_count > 1) {
            /* Only bother introducing the hidden shared-source binding
             * when there's more than one name to share it between -- a
             * single-name `inherit (expr) x;` already evaluates `expr`
             * exactly once either way (there's only ever the one
             * `AST_SELECT` referencing it), so leaving it exactly as
             * before for that case sidesteps a real complication: a
             * `rec {}` combining `__overrides` with this hidden mechanism
             * needs more care than this pass attempts (see compiler.c's
             * AST_ATTRSET/overrides_slot comment). This `> 1` guard keeps
             * the single-name case on the untouched, already-correct
             * path and avoids that conflict entirely rather than papering
             * over it. */
            char *hidden_name = malloc(32);
            int hlen = snprintf(hidden_name, 32, "\x01inherit#%d", g_inherit_hidden_counter++);
            BTNode hidden_node = {
                .name = hidden_name, .name_len = (size_t)hlen,
                .is_leaf = 1, .leaf_value = src_expr, .is_inherit = 0, .is_hidden = 1 };
            btvec_push(out, hidden_node);
            Ast *v = ll_ast_new_at(AST_VAR, cur(p).start);
            v->as.var.name = hidden_name;
            v->as.var.name_len = (size_t)hlen;
            hidden_var = v;
        }
        for (size_t ni = 0; ni < names_count; ni++) {
            AttrSeg name = names_items[ni];
            Ast *value;
            if (src_expr) {
                /* Single-name `inherit (expr) x;` (no hidden binding was
                 * created -- `names_count` wasn't `> 1`) still selects off
                 * `src_expr` directly; a multi-name clause selects off the
                 * shared `hidden_var` instead. Do NOT check `if
                 * (hidden_var)` here instead of `if (src_expr)`: that
                 * makes a single-name `inherit (x) c;` fall into the
                 * bare-`inherit` branch below and compile `c`'s value as
                 * a plain `AST_VAR("c")` -- which, looked up against this
                 * same block's own recursive frame, resolves right back
                 * to itself: `c`'s own thunk forcing `c`'s own thunk, a
                 * genuine blackhole (confirmed via numtide/nix-
                 * conformance's own eval-okay-inherit-from.nix). */
                Ast *sel = ll_ast_new_at(AST_SELECT, cur(p).start);
                sel->as.select.target = hidden_var ? hidden_var : src_expr;
                sel->as.select.attr = name.attr;
                sel->as.select.attr_len = name.attr_len;
                sel->as.select.attr_expr = NULL;
                sel->as.select.default_ = NULL;
                value = sel;
            } else {
                Ast *v = ll_ast_new_at(AST_VAR, cur(p).start);
                v->as.var.name = name.attr;
                v->as.var.name_len = name.attr_len;
                value = v;
            }
            /* `inherit (expr) x;` is sugar for an ordinary binding `x =
             * expr.x;` -- expr resolves against this block's own scope
             * just like any other binding's value (siblings visible).
             * Only the bare `inherit x;` form needs the is_inherit
             * escape-to-outer-scope treatment below (see ast.h) -- real
             * Nix resolves a bare inherited name against the scope
             * *enclosing* this block, not the block's own recursive
             * scope, even though plain `x` would otherwise self-match.
             * An inherited name is always a single segment in real Nix's
             * own grammar (`attrs: attrs attr | ...`, never `attrpath`),
             * so it always goes through insert_attrpath as a length-1
             * path -- still subject to the same dup-attr/merge rules as
             * an ordinary binding if it collides with one. The
             * *synthetic* hidden binding above bypasses insert_attrpath
             * entirely instead (pushed straight to `out`) -- see its own
             * comment on why that's safe. */
            insert_attrpath(out, &name, 1, 0, value, (src_expr == NULL));
        }
        free(names_items); /* insert_attrpath copies each AttrSeg by value -- nothing retains `names_items` itself */
        expect(p, TOK_SEMI, "';'");
        return;
    }
    /* Attrpath binding sugar (`a.b.c = val;`, real Nix's `attrpath '='
     * expr ';'`) -- see the BTNode/insert_attrpath comment above for the
     * full merge semantics. Each segment may be a plain identifier, a
     * static quoted-string key, or a dynamic one (bare `${expr}` or an
     * interpolated quoted string) anywhere in the chain, including as
     * the very first segment -- see parse_attr_segment. Collect every
     * segment first, then split at the first dynamic one (if any): the
     * static prefix (possibly empty) still flows through insert_attrpath
     * as before; everything from the first dynamic segment onward
     * becomes a synthetic value via build_dynamic_chain_value, which
     * either becomes that static prefix's leaf value (non-empty prefix)
     * or -- when the very first segment is itself dynamic -- a direct
     * push to `dyn_out` (same `NULL` rejection as a bare `${expr} =
     * value;` binding: "dynamic attributes not allowed in let"). */
    size_t segs_mark = attrsegvec_mark();
    attrsegvec_push(parse_attr_segment(p, "a binding name"));
    while (cur(p).kind == TOK_DOT) {
        advance(p);
        attrsegvec_push(parse_attr_segment(p, "an attribute name"));
    }
    expect(p, TOK_EQUALS, "'='");
    /* Real source byte offset of this RHS's first token -- see the
     * marker-reparse comment on g_inherit_hidden_counter above. Captured
     * unconditionally (cheap: one extra local + field write per
     * attrset/let binding, gated on the one flag that makes it matter)
     * so a later cache-store of a not-yet-forced lazy-thunk stub wrapping
     * THIS exact value can record a byte offset that reparses back to an
     * equivalent Ast, instead of bytecode.c's pre-existing hard refusal
     * to cache any program containing an unresolved stub at all. */
    uint32_t value_src_pos = cur(p).start;
    Ast *value = parse_expr(p); /* may recurse into another attrsegvec user -- that fully
                                  * commits and unwinds before returning, so it's still safe
                                  * to commit THIS span only now, below (see SCRATCH_VEC's
                                  * own safety argument) */
    /* Only this exact leaf value -- not the synthetic nested AST_ATTRSET
     * wrappers attrpath sugar (`a.b.c = v;`) builds around it via
     * insert_attrpath/finalize_bt_node below -- corresponds to a real,
     * verbatim, standalone-parseable span of the source buffer (the
     * wrappers don't appear in source at all, e.g. `a.b.c = v;` has no
     * literal `{ b = { c = v; }; }` text anywhere); those keep src_pos's
     * default 0 ("no known span"), which bytecode.c treats as "fall back
     * to the old refuse-to-cache behavior for this stub" -- see that
     * file's PTAG_OP_LAZY_MARKER comment. */
    if (ll_lazy_thunk_enabled) value->src_pos = value_src_pos;
    expect(p, TOK_SEMI, "';'");

    size_t segs_count;
    AttrSeg *segs_items = attrsegvec_commit(segs_mark, &segs_count);
    size_t first_dyn = segs_count;
    for (size_t i = 0; i < segs_count; i++) {
        if (segs_items[i].is_dynamic) { first_dyn = i; break; }
    }
    if (first_dyn == segs_count) {
        /* Fully static -- unchanged original behavior. */
        insert_attrpath(out, segs_items, segs_count, 0, value, 0);
    } else if (first_dyn == 0) {
        if (!dyn_out) {
            fprintf(stderr, "ll: parse error: dynamic attributes not allowed in let\n");
            exit(1);
        }
        Ast *inner = build_dynamic_chain_value(segs_items, 1, segs_count - 1, value);
        dynbindingvec_push(dyn_out, (DynBinding){ .name_expr = segs_items[0].name_expr, .value = inner });
    } else {
        Ast *inner = build_dynamic_chain_value(segs_items, first_dyn, segs_count - first_dyn, value);
        insert_attrpath(out, segs_items, first_dyn, 0, inner, 0);
    }
    free(segs_items);
}

/* An interpolated path literal's tail -- `t` is the already-consumed
 * first TOK_PATH chunk (e.g. "./" in "./${foo}", "~/" in "~/${foo}"),
 * known by the caller to be immediately followed by TOK_DOLLAR_LBRACE.
 * Collects zero or more TOK_PATH_PART literal chunks interleaved with
 * TOK_DOLLAR_LBRACE ... TOK_RBRACE interpolated expressions (same shape
 * parse_dquote_string uses for TOK_STR_PART, reusing AstStringPart
 * directly) up to the closing TOK_PATH_END, and builds an AST_PATH node
 * whose `text`/`len`/`is_home` are the first chunk and whose
 * `interp_parts`/`interp_count` hold everything after it -- see
 * compiler.c's AST_PATH case (resolve_path_chunk) for how the first
 * chunk alone becomes a Path constant and the rest becomes a runtime
 * OP_PATH_CONCAT chain. */
static Ast *parse_path_literal_interp(Parser *p, Token t) {
    Ast *n = ll_ast_new_at(AST_PATH, cur(p).start);
    n->as.path.text = tok_text(p, t);
    n->as.path.len = tok_len(t);
    n->as.path.is_search = 0;
    n->as.path.is_home = (p->src[t.start] == '~'); /* derived, not carried on Token (see lexer.h) */

    size_t parts_mark = partvec_mark();
    while (cur(p).kind != TOK_PATH_END) {
        if (cur(p).kind == TOK_PATH_PART) {
            Token pt = cur(p);
            partvec_push((AstStringPart){ .is_expr = 0, .text = tok_text(p, pt), .text_len = tok_len(pt) });
            advance(p);
        } else if (cur(p).kind == TOK_DOLLAR_LBRACE) {
            advance(p);
            Ast *e = parse_expr(p);
            expect(p, TOK_RBRACE, "'}' closing a path interpolation");
            partvec_push((AstStringPart){ .is_expr = 1, .expr = e });
        } else {
            fprintf(stderr, "ll: parse error: malformed path literal\n");
            exit(1);
        }
    }
    advance(p); /* TOK_PATH_END */
    size_t parts_count;
    AstStringPart *parts_items = partvec_commit(parts_mark, &parts_count);
    n->as.path.interp_parts = parts_items;
    n->as.path.interp_count = parts_count;
    return n;
}

/* "foo ${expr} bar": zero or more TOK_STR_PART literal chunks interleaved
 * with TOK_DOLLAR_LBRACE ... TOK_RBRACE interpolated expressions, parsed
 * at full `expr` precedence (so e.g. `"${a} ${b}"` or `"${if c then x else
 * y}"` both just work -- the lexer's mode stack is what makes re-entering
 * parse_expr here safe, see lexer.c). */
static Ast *parse_dquote_string(Parser *p) {
    advance(p); /* TOK_STR_START */
    size_t parts_mark = partvec_mark();
    while (cur(p).kind != TOK_STR_END) {
        if (cur(p).kind == TOK_STR_PART) {
            /* TOK_STR_PART is a raw (escapes-not-yet-applied) span --
             * decode exactly once, right here, at the point this AstStringPart
             * is actually built (see ll_decode_dquote_span's own comment,
             * lexer.h). */
            Token t = cur(p);
            size_t dlen;
            char *dtext = ll_decode_dquote_span(p->src, t.start, t.end, &dlen);
            partvec_push((AstStringPart){ .is_expr = 0, .text = dtext, .text_len = dlen });
            advance(p);
        } else if (cur(p).kind == TOK_DOLLAR_LBRACE) {
            advance(p);
            Ast *e = parse_expr(p);
            expect(p, TOK_RBRACE, "'}' closing a string interpolation");
            partvec_push((AstStringPart){ .is_expr = 1, .expr = e });
        } else {
            fprintf(stderr, "ll: parse error: malformed string literal\n");
            exit(1);
        }
    }
    advance(p); /* TOK_STR_END */
    size_t parts_count;
    AstStringPart *parts_items = partvec_commit(parts_mark, &parts_count);
    Ast *n = ll_ast_new_at(AST_STRING, cur(p).start);
    n->as.string.parts = parts_items;
    n->as.string.count = parts_count;
    return n;
}

/* '' ... '' indented string: same interpolation shape as above, but the
 * literal chunks are raw (un-indentation-stripped) and get run through
 * strip_indentation() once the whole literal is collected -- see that
 * function's comment for why this has to happen only after collecting
 * every part (the minimum indentation is computed across ALL of them). */
static Ast *parse_indented_string(Parser *p) {
    advance(p); /* TOK_ISTR_START */
    size_t raw_mark = rawind_mark();
    while (cur(p).kind != TOK_ISTR_END) {
        if (cur(p).kind == TOK_ISTR_PART) {
            /* A raw content run never has escapes in it by construction
             * (an escape always ends the run, see lex_indented_run) -- its
             * span IS its final text verbatim, no decode and no copy
             * needed at all (points straight into the long-lived source
             * buffer). */
            Token t = cur(p);
            rawind_push((RawIndPart){ .is_expr = 0, .text = tok_text(p, t), .text_len = tok_len(t),
                                       .has_indentation = 1 });
            advance(p);
        } else if (cur(p).kind == TOK_ISTR_ESC) {
            /* One `''$`/`'''`/`''\<c>` escape unit -- decode now, into a
             * freshly malloc'd (<=2-byte) owned buffer, matching the owned-
             * copy convention every other RawIndPart.text already relies on
             * (strip_indentation never frees these directly, but the whole
             * raw_items array -- and every RawIndPart.text it points at --
             * is freed together right after strip_indentation runs, see
             * parse_indented_string below; the TOK_ISTR_PART case above is
             * the one deliberate exception, since IT points into source,
             * which this file never frees). */
            Token t = cur(p);
            char decoded[2];
            size_t dlen;
            ll_decode_istr_esc(p->src, t.start, decoded, &dlen);
            char *owned = malloc(dlen);
            memcpy(owned, decoded, dlen);
            rawind_push((RawIndPart){ .is_expr = 0, .text = owned, .text_len = dlen, .has_indentation = 0 });
            advance(p);
        } else if (cur(p).kind == TOK_DOLLAR_LBRACE) {
            advance(p);
            Ast *e = parse_expr(p);
            expect(p, TOK_RBRACE, "'}' closing a string interpolation");
            rawind_push((RawIndPart){ .is_expr = 1, .expr = e });
        } else {
            fprintf(stderr, "ll: parse error: malformed indented string literal\n");
            exit(1);
        }
    }
    advance(p); /* TOK_ISTR_END */

    size_t raw_count;
    RawIndPart *raw_items = rawind_commit(raw_mark, &raw_count);
    size_t count;
    AstStringPart *parts = strip_indentation(raw_items, raw_count, &count);
    free(raw_items);

    Ast *n = ll_ast_new_at(AST_STRING, cur(p).start);
    n->as.string.parts = parts;
    n->as.string.count = count;
    return n;
}

static Ast *parse_atom(Parser *p) {
    Token t = cur(p);
    switch (t.kind) {
        case TOK_INT: {
            advance(p);
            Ast *n = ll_ast_new_at(AST_INT, cur(p).start);
            n->as.int_val = strtoll(p->src + t.start, NULL, 10);
            return n;
        }
        case TOK_FLOAT: {
            advance(p);
            Ast *n = ll_ast_new_at(AST_FLOAT, cur(p).start);
            n->as.float_val = ll_decode_float_literal(p->src, t.start, t.end);
            return n;
        }
        case TOK_TRUE: {
            advance(p);
            Ast *n = ll_ast_new_at(AST_BOOL, cur(p).start);
            n->as.bool_val = 1;
            return n;
        }
        case TOK_FALSE: {
            advance(p);
            Ast *n = ll_ast_new_at(AST_BOOL, cur(p).start);
            n->as.bool_val = 0;
            return n;
        }
        case TOK_NULL: {
            advance(p);
            return ll_ast_new_at(AST_NULL, cur(p).start);
        }
        case TOK_STR_START:
            return parse_dquote_string(p);
        case TOK_ISTR_START:
            return parse_indented_string(p);
        case TOK_IDENT:
        case TOK_OR: { /* bare `or` as an ordinary (deprecated/"cursed") variable -- see starts_atom's comment */
            /* `__curPos` is a genuine special FORM in real Nix's own
             * grammar (`parser.y`'s `IND_STRING | ... | '__curPos'`
             * production builds an `ExprPos` node directly, not an
             * ordinary variable reference -- `let __curPos = 1; in
             * __curPos` still evaluates to the POSITION of that
             * `__curPos` token, never binding to the shadowing `let`),
             * so it's intercepted here, before ever becoming an ordinary
             * AST_VAR. */
            if (t.kind == TOK_IDENT && tok_len(t) == 8 && memcmp(tok_text(p, t), "__curPos", 8) == 0) {
                advance(p);
                Ast *n = ll_ast_new_at(AST_CURPOS, cur(p).start);
                curpos_line_col(p->curpos_base, p->src + t.start, &n->as.curpos.line, &n->as.curpos.column);
                return n;
            }
            advance(p);
            Ast *n = ll_ast_new_at(AST_VAR, cur(p).start);
            n->as.var.name = tok_text(p, t);
            n->as.var.name_len = tok_len(t);
            return n;
        }
        case TOK_PATH: {
            advance(p);
            /* The lexer only ever follows a TOK_PATH with
             * TOK_DOLLAR_LBRACE when this chunk is the first (possibly
             * only) part of a path literal that continues into
             * interpolation (`./${foo}`, `~/${foo}`, ...) -- see
             * lexer.c's at_dollar_brace checks in ll_lex's main loop.
             * A bare "${" can't otherwise appear here at all (it's not
             * valid Nix syntax outside a string/path literal), so this
             * peek is unambiguous. */
            if (cur(p).kind == TOK_DOLLAR_LBRACE) {
                return parse_path_literal_interp(p, t);
            }
            Ast *n = ll_ast_new_at(AST_PATH, cur(p).start);
            n->as.path.text = tok_text(p, t);
            n->as.path.len = tok_len(t);
            n->as.path.is_search = 0;
            n->as.path.is_home = (p->src[t.start] == '~'); /* derived, see lexer.h */
            n->as.path.interp_parts = NULL;
            n->as.path.interp_count = 0;
            return n;
        }
        case TOK_URI: {
            /* Bare URL literal -- lexes to a plain, non-interpolated
             * string (real Nix: `ExprString` built straight from the
             * literal's text, same AST shape a `"..."` with no `${`
             * ever produces). See lexer.c's try_scan_uri for the
             * grammar/precedence rationale. */
            advance(p);
            Ast *n = ll_ast_new_at(AST_STRING, cur(p).start);
            AstStringPart *parts = malloc(sizeof(AstStringPart));
            parts[0].is_expr = 0;
            parts[0].text = tok_text(p, t);
            parts[0].text_len = tok_len(t);
            parts[0].expr = NULL;
            n->as.string.parts = parts;
            n->as.string.count = 1;
            return n;
        }
        case TOK_SEARCH_PATH: {
            advance(p);
            Ast *n = ll_ast_new_at(AST_PATH, cur(p).start);
            n->as.path.text = tok_text(p, t);
            n->as.path.len = tok_len(t);
            n->as.path.is_search = 1;
            n->as.path.is_home = 0;
            n->as.path.interp_parts = NULL;
            n->as.path.interp_count = 0;
            return n;
        }
        case TOK_LPAREN: {
            advance(p);
            Ast *e = parse_expr(p);
            expect(p, TOK_RPAREN, "')'");
            return e;
        }
        case TOK_LBRACKET: {
            advance(p);
            size_t elems_mark = astvec_mark();
            while (cur(p).kind != TOK_RBRACKET) astvec_push(parse_select(p));
            advance(p); /* ']' */
            size_t elems_count;
            Ast **elems_items = astvec_commit(elems_mark, &elems_count);
            Ast *n = ll_ast_new_at(AST_LIST, cur(p).start);
            n->as.list.elems = elems_items;
            n->as.list.count = elems_count;
            return n;
        }
        case TOK_LBRACE: {
            advance(p);
            BTVec bt = { 0 };
            DynBindingVec dyn = { 0 };
            /* Lazy-thunk-compile experiment, ported from asnix: wraps
             * the WHOLE literal's parse (not just parse_binding_stmt's
             * own already-wrapped per-binding value), since
             * finalize_bt_vec/merge_bt_dyn_into build wrapper Ast nodes
             * AFTER the loop (e.g. a dynamic-attr merge) that also
             * become part of a deferred field's body. */
            int lt_prev_lbrace = ll_ast_enter_retained();
            while (cur(p).kind != TOK_RBRACE) parse_binding_stmt(p, &bt, &dyn);
            advance(p); /* '}' */
            BindingVec bindings = { 0 };
            finalize_bt_vec(&bt, &bindings);
            VEC_TRIM(bindings, AstBinding);
            Ast *n = ll_ast_new_at(AST_ATTRSET, cur(p).start);
            n->as.attrset.bindings = bindings.items;
            n->as.attrset.count = bindings.len;
            n->as.attrset.is_rec = 0;
            merge_bt_dyn_into(&bt, &dyn);
            n->as.attrset.dyn_bindings = finalize_dyn_vec(&dyn);
            n->as.attrset.dyn_count = dyn.len;
            ll_ast_leave_retained(lt_prev_lbrace);
            return n;
        }
        case TOK_REC: {
            advance(p);
            expect(p, TOK_LBRACE, "'{'");
            BTVec bt = { 0 };
            DynBindingVec dyn = { 0 };
            int lt_prev_rec = ll_ast_enter_retained(); /* see TOK_LBRACE's own comment above */
            while (cur(p).kind != TOK_RBRACE) parse_binding_stmt(p, &bt, &dyn);
            advance(p); /* '}' */
            BindingVec bindings = { 0 };
            finalize_bt_vec(&bt, &bindings);
            VEC_TRIM(bindings, AstBinding);
            Ast *n = ll_ast_new_at(AST_ATTRSET, cur(p).start);
            n->as.attrset.bindings = bindings.items;
            n->as.attrset.count = bindings.len;
            n->as.attrset.is_rec = 1;
            merge_bt_dyn_into(&bt, &dyn);
            n->as.attrset.dyn_bindings = finalize_dyn_vec(&dyn);
            n->as.attrset.dyn_count = dyn.len;
            ll_ast_leave_retained(lt_prev_rec);
            return n;
        }
        default:
            fprintf(stderr, "ll: parse error: unexpected token\n");
            exit(1);
    }
}

static Ast *parse_select(Parser *p) {
    Ast *e = parse_atom(p);
    int had_select = 0;
    while (cur(p).kind == TOK_DOT) {
        advance(p);
        Ast *n = ll_ast_new_at(AST_SELECT, cur(p).start);
        n->as.select.target = e;
        n->as.select.default_ = NULL;
        /* Dynamic attribute name, `.${expr}` or an interpolated quoted
         * string (real Nix's grammar for a computed-at-runtime attrpath
         * segment, e.g. `set.${name}`), or a plain identifier/static
         * quoted-string segment (`x."a b"`) -- see parse_attr_segment's
         * comment. `or` after a dynamic segment (`set.${name} or
         * default`) is a real, supported combination -- handled
         * uniformly below by the same `had_select && TOK_OR` check a
         * static segment uses, compiler.c's AST_SELECT case picks
         * OP_SELECT_DYNAMIC_OR instead of the static OP_SELECT_OR when
         * attr_expr is set. */
        AttrSeg seg = parse_attr_segment(p, "an attribute name");
        set_select_segment(n, seg);
        e = n;
        had_select = 1;
    }
    /* `expr.attr or default` -- real Nix's grammar attaches `or` to the
     * whole preceding `expr '.' attrpath`, not per-segment, and
     * `ExprSelect::eval` (eval.cc) evaluates that whole multi-segment
     * attrpath in ONE loop, falling through to `default` the instant ANY
     * segment is missing or the current value isn't an attrset -- not
     * just the last one. This parser instead builds a chain of nested
     * single-segment AST_SELECT nodes (one per `.segment`, `target`
     * pointing at the previous node), so matching that semantics means
     * sharing the SAME `default_` Ast* across every node of this one
     * unbroken chain, not just the outermost (last) one -- walk back down
     * `target` while it's itself a freshly-built AST_SELECT (default_ ==
     * NULL, true for every node of THIS chain and only this chain, by
     * construction: an earlier chain's `or` would already have set it)
     * and stamp all of them with the same pointer. Sharing one Ast*
     * subtree across multiple parent nodes is safe here -- compiler.c
     * only ever READS `default_` to emit a fresh, independent copy of
     * its bytecode at each compile site, never mutates the AST, so a
     * side-effecting default (`a.x.y or (builtins.trace "eval"
     * default)`) still evaluates exactly once, not once per missing
     * segment. `default` itself is parsed at this same parse_select
     * tightness (matching real Nix: the default is NOT a full `expr`,
     * e.g. `+`/`==` outside are not absorbed into it), recursively, so
     * `a.b or c.d or e` chains sensibly. No `or` is possible without at
     * least one preceding `.select` (real Nix's grammar agrees -- a bare
     * `x or y` is the separate, unsupported "cursed or" rule). */
    if (had_select && cur(p).kind == TOK_OR) {
        advance(p);
        Ast *default_ast = parse_select(p);
        Ast *chain_node = e;
        while (chain_node->kind == AST_SELECT && chain_node->as.select.default_ == NULL) {
            chain_node->as.select.default_ = default_ast;
            Ast *next = chain_node->as.select.target;
            if (next->kind != AST_SELECT) break;
            chain_node = next;
        }
    }
    return e;
}

static Ast *parse_app(Parser *p) {
    Ast *fn = parse_select(p);
    while (starts_atom(cur(p).kind)) {
        Ast *arg = parse_select(p);
        Ast *app = ll_ast_new_at(AST_APP, cur(p).start);
        app->as.app.fn = fn;
        app->as.app.arg = arg;
        fn = app;
    }
    return fn;
}

/* Unary `-` (negate): binds tighter than every binary/`?`/`++` operator
 * but looser than application/select -- `-f 3` == `-(f 3)` (not `(-f)
 * 3`), `-x ? "a"` == `(-x) ? "a"` (not `-(x ? "a")`), `-2 + 3` == `(-2) +
 * 3` == `1`. Matches real Nix's grammar (`~/nix/src/libexpr/parser.y`:
 * `'-' expr_op %prec NEGATE` desugars to `sub 0 $2`, i.e. `0 - expr_op`
 * -- NEGATE is the single tightest-declared operator in the whole
 * table, but still looser than implicit application/select, which
 * aren't part of that table at all). Reuses the existing binary `-`
 * (AST_BINOP op='-') with a literal `0` lhs, same desugaring real Nix
 * itself uses -- no new AST node or IR op needed. */
static Ast *parse_binop(Parser *p, int min_prec); /* forward decl -- see parse_negate's own `!` case below */
#define PREC_ADD 6 /* must match TOK_PLUS/TOK_MINUS's own `prec` entry in the BINOPS table, further down this file -- moved up here (not duplicated) since parse_negate's own `!`-operand case below needs it */
static Ast *parse_negate(Parser *p) {
    if (cur(p).kind == TOK_MINUS) {
        advance(p);
        Ast *operand = parse_negate(p); /* right-recurse: allows `--x`, harmless */
        Ast *zero = ll_ast_new_at(AST_INT, cur(p).start);
        zero->as.int_val = 0;
        Ast *n = ll_ast_new_at(AST_BINOP, cur(p).start);
        n->as.binop.op = '-';
        n->as.binop.lhs = zero;
        n->as.binop.rhs = operand;
        return n;
    }
    /* Correctness fix: `-`'s own operand (just above) can start with `!`
     * too (`-!x`, `-!-x`, ...) -- real Nix accepts this syntactically,
     * failing only at runtime on `-`'s own operand-type check, never at
     * parse time (confirmed: `nix-instantiate --eval '(-!true)'`
     * evaluates to a type error, not a parse error). `parse_binop`'s own
     * top-level `!` check (above, where this whole chain is entered) only
     * ever runs ONCE, before `-` is consumed -- so it can't see this `!`,
     * which only appears once we're already recursing into `-`'s operand
     * position. Mirrors that same top-level check's own operand
     * precedence (`PREC_ADD`) and AST shape exactly; harmless/unreached
     * when `parse_negate` is entered directly (no leading `-` already
     * consumed), since the top-level check above would have intercepted
     * a leading `!` there first. */
    if (cur(p).kind == TOK_BANG) {
        advance(p);
        Ast *operand = parse_binop(p, PREC_ADD);
        Ast *n = ll_ast_new_at(AST_NOT, cur(p).start);
        n->as.not1.operand = operand;
        return n;
    }
    return parse_app(p);
}

/* `expr ? attr` (has-attr): binds tighter than arithmetic but looser than
 * application, matching real Nix's precedence table. Confirmed against
 * `nix-instantiate --eval '1 ? a'` => false (not an error) -- a non-
 * attrset left-hand side is simply "no", see op_has_attr.S. */
static Ast *parse_has_attr(Parser *p) {
    Ast *e = parse_negate(p);
    while (cur(p).kind == TOK_QUESTION) {
        advance(p);
        /* `expr ? attrpath`, real Nix's grammar for the right-hand side
         * (`~/nix/src/libexpr/parser.y`: `expr_op '?' attrpath`) is a
         * full DOTTED chain, not a single segment -- `x ? a.b.c` checks
         * nested existence. Each segment may be a plain identifier, a
         * static quoted-string, or a dynamic `${expr}` (see
         * parse_attr_segment/AttrSeg above). Desugars directly to `x ? a
         * && (x.a or {}) ? b && ((x.a or {}).b or {}) ? c ...` -- reusing
         * the already-correct AST_HAS_ATTR/AST_SELECT-with-`or`/AST_AND
         * nodes, no new AST node needed (dynamic segments needed one new
         * IR op, OP_HAS_ATTR_DYNAMIC, since AST_SELECT already had its
         * own dynamic op). This matches real Nix for the three cases
         * that actually distinguish it from a naive "just select, no
         * fallback" version: a missing intermediate attr, an
         * intermediate value that exists but isn't an attrset, and a
         * fully-present chain -- all graceful (`false`), none fatal. */
        AttrSeg seg = parse_attr_segment(p, "an attribute name");
        Ast *n = ll_ast_new_at(AST_HAS_ATTR, cur(p).start);
        n->as.has_attr.target = e;
        set_has_attr_segment(n, seg);
        Ast *result = n;
        Ast *running_target = e;
        while (cur(p).kind == TOK_DOT) {
            advance(p);
            Ast *sel = ll_ast_new_at(AST_SELECT, cur(p).start);
            sel->as.select.target = running_target;
            set_select_segment(sel, seg);   /* the segment just checked above */
            Ast *empty_attrs = ll_ast_new_at(AST_ATTRSET, cur(p).start);
            empty_attrs->as.attrset.bindings = NULL;
            empty_attrs->as.attrset.count = 0;
            empty_attrs->as.attrset.is_rec = 0;
            empty_attrs->as.attrset.dyn_bindings = NULL;
            empty_attrs->as.attrset.dyn_count = 0;
            sel->as.select.default_ = empty_attrs;
            running_target = sel; /* == the previous target's `.attr or {}` */

            seg = parse_attr_segment(p, "an attribute name");
            Ast *next_check = ll_ast_new_at(AST_HAS_ATTR, cur(p).start);
            next_check->as.has_attr.target = running_target;
            set_has_attr_segment(next_check, seg);

            Ast *and_node = ll_ast_new_at(AST_AND, cur(p).start);
            and_node->as.logic.lhs = result;
            and_node->as.logic.rhs = next_check;
            result = and_node;
        }
        e = result;
    }
    return e;
}

/* `++`/`*`//`+`/`-` (list concat, mul/div, add/sub): folded into the
 * table-driven `parse_binop` climbing loop below, alongside `//`/`==`/
 * `!=`/`<`/`>`/`<=`/`>=`/`&&`/`||`/`->` -- see that function's own header
 * comment for the precedence table. `?` (has-attr, tighter than `++`)
 * keeps its own real grammar in `parse_has_attr` below, unchanged. */

/* Operator precedence below `parse_expr`'s keyword forms (`let`/`if`/
 * lambda/`assert`), lowest to highest. This is NOT the same ordering a
 * literal reading of the Nix manual's summary table might suggest for
 * `!`, which binds looser than `+`/`-` but tighter than `//`: `! 1 + 1`
 * parses as `!(1 + 1)`, and `! true // {}` parses as `(!true) // {}`:
 *
 *   ->           right-assoc, chainable, LOWEST of all of these
 *   ||           left-assoc, chainable
 *   &&           left-assoc, chainable
 *   == !=        non-assoc (single occurrence only)
 *   < > <= >=    non-assoc (single occurrence only)
 *   //           right-assoc, chainable
 *   ! (prefix)   binds the rest of this chain down to PREC_ADD
 *   + -
 *   * /
 *   ++           right-assoc, chainable -- binds TIGHTER than
 *                 * / + -, looser than ? and application
 *
 * `->`'s precedence/associativity (lowest, right-assoc) matches cppnix's
 * real grammar (~/nix/src/libexpr/parser.y: `%right IMPL`, declared
 * before `%left OR`, i.e. binding looser).
 *
 * This whole chain is a single table-driven climbing loop (`parse_binop`)
 * rather than one recursive-descent function per precedence level --
 * `parse_has_attr` and everything tighter (`?`, unary `-`/negate,
 * application, select, atom) keep their own real, non-table grammar as
 * separate functions, unchanged below -- only the "plain binary
 * operator" levels are table-driven. */
typedef enum { BINOP_K_BINOP, BINOP_K_AND, BINOP_K_OR, BINOP_K_IMPL } BinOpKind;
typedef struct {
    TokKind tok;
    int prec;
    int right_assoc; /* chainable via same-level RHS recursion */
    int non_assoc;   /* exactly one occurrence allowed -- break after firing once */
    BinOpKind kind;
    char op; /* only meaningful when kind == BINOP_K_BINOP */
} BinOpInfo;

/* Precedence values are this table's OWN numbering (not tied to any
 * other representation) -- only their relative ORDER matters, lowest
 * (loosest) to highest (tightest), matching the comment block above
 * exactly. PREC_ADD below must stay in sync with `+`/`-`'s own `prec`
 * value -- it's `!`'s own operand boundary (see parse_binop_primary). */
static const BinOpInfo BINOPS[] = {
    { TOK_ARROW,      0, 1, 0, BINOP_K_IMPL,  0   },
    { TOK_OROR,       1, 0, 0, BINOP_K_OR,    0   },
    { TOK_ANDAND,     2, 0, 0, BINOP_K_AND,   0   },
    { TOK_EQEQ,       3, 0, 1, BINOP_K_BINOP, 'e' },
    { TOK_NEQ,        3, 0, 1, BINOP_K_BINOP, 'n' },
    { TOK_LT,         4, 0, 1, BINOP_K_BINOP, 'l' },
    { TOK_GT,         4, 0, 1, BINOP_K_BINOP, 'g' },
    { TOK_LE,         4, 0, 1, BINOP_K_BINOP, 'L' },
    { TOK_GE,         4, 0, 1, BINOP_K_BINOP, 'G' },
    { TOK_SLASHSLASH, 5, 1, 0, BINOP_K_BINOP, 'u' },
    { TOK_PLUS,       6, 0, 0, BINOP_K_BINOP, '+' },
    { TOK_MINUS,      6, 0, 0, BINOP_K_BINOP, '-' },
    { TOK_STAR,       7, 0, 0, BINOP_K_BINOP, '*' },
    { TOK_SLASH,      7, 0, 0, BINOP_K_BINOP, '/' },
    { TOK_PLUSPLUS,   8, 1, 0, BINOP_K_BINOP, 'c' },
};
#define BINOPS_COUNT (sizeof(BINOPS) / sizeof(BINOPS[0]))
/* PREC_ADD itself is defined earlier in this file, right before
 * parse_negate (which needs it for its own `!`-operand case) -- kept in
 * sync with TOK_PLUS/TOK_MINUS's own `prec` entry above by that
 * definition's own comment, not duplicated here. */

static const BinOpInfo *lookup_binop(TokKind k) {
    for (size_t i = 0; i < BINOPS_COUNT; i++) {
        if (BINOPS[i].tok == k) return &BINOPS[i];
    }
    return NULL;
}

static Ast *make_binop_node(const BinOpInfo *info, Ast *lhs, Ast *rhs) {
    switch (info->kind) {
        case BINOP_K_AND: {
            Ast *n = ll_ast_new(AST_AND);
            n->as.logic.lhs = lhs;
            n->as.logic.rhs = rhs;
            return n;
        }
        case BINOP_K_OR: {
            Ast *n = ll_ast_new(AST_OR);
            n->as.logic.lhs = lhs;
            n->as.logic.rhs = rhs;
            return n;
        }
        case BINOP_K_IMPL: {
            /* `a -> b` desugars directly to `(!a) || b` -- exactly real
             * Nix's own ExprOpImpl::eval semantics (`!evalBool(e1) ||
             * evalBool(e2)`, cppnix's eval.cc), so this gets short-
             * circuiting (b is never evaluated when a is false) for free
             * by reusing the already-correct AST_OR/AST_NOT compilation,
             * no new AST node or IR op needed. */
            Ast *not_lhs = ll_ast_new(AST_NOT);
            not_lhs->as.not1.operand = lhs;
            Ast *n = ll_ast_new(AST_OR);
            n->as.logic.lhs = not_lhs;
            n->as.logic.rhs = rhs;
            return n;
        }
        case BINOP_K_BINOP:
        default: {
            Ast *n = ll_ast_new(AST_BINOP);
            n->as.binop.op = info->op;
            n->as.binop.lhs = lhs;
            n->as.binop.rhs = rhs;
            return n;
        }
    }
}

static Ast *parse_binop(Parser *p, int min_prec);

/* The "primary" step every climbing-loop call starts with: `!` (prefix)
 * first -- its own operand is parsed at PREC_ADD, i.e. everything this
 * table drives at `+`'s own precedence or tighter (+/-, * /, ++), so
 * `!1 + 1` consumes the whole `1 + 1` as one operand (matching real Nix,
 * see the comment above), while `!a // b` stops before `//` (prec 5 <
 * PREC_ADD) and parses as `(!a) // b`, not `!(a // b)`. A second `!` is
 * caught the same way regardless of nesting depth, since this function
 * re-checks TOK_BANG on every call, including the recursive one for the
 * operand -- `!!x` works via that re-entry, not a dedicated loop. Falls
 * through to `parse_has_attr` (its own real, non-table grammar: `?`'s
 * dotted-chain desugaring, then unary negate, application, select,
 * atom) for everything else. */
static Ast *parse_binop_primary(Parser *p) {
    if (cur(p).kind == TOK_BANG) {
        advance(p);
        Ast *operand = parse_binop(p, PREC_ADD);
        Ast *n = ll_ast_new_at(AST_NOT, cur(p).start);
        n->as.not1.operand = operand;
        return n;
    }
    return parse_has_attr(p);
}

/* Classic precedence climbing: parses one primary, then repeatedly
 * consumes an infix operator whose own precedence is >= min_prec,
 * recursing for its RHS at a bumped-up min_prec (right_assoc: SAME
 * level, allowing further right-recursion at that level, which is what
 * makes e.g. `a -> b -> c` associate as `a -> (b -> c)`; otherwise
 * prec+1, forcing the RHS to bind tighter so the loop's own next
 * iteration -- not a nested call -- picks up the next same-level
 * operator, which is what makes e.g. `a + b + c` associate as
 * `(a+b)+c`). `non_assoc` breaks the loop immediately after firing once
 * -- the leftover token (e.g. the second `==` in `1==2==3`) is simply
 * never consumed by anything in this chain, surfacing as a parse error
 * wherever control eventually returns to. */
static Ast *parse_binop(Parser *p, int min_prec) {
    Ast *lhs = parse_binop_primary(p);
    /* Tracks the precedence of the last NON-ASSOC operator fired in THIS
     * loop (-1 == none yet) -- a non-assoc operator may only forbid a
     * repeat at its OWN exact precedence (`1 < 2 < 3`, `1 == 2 == 3`:
     * both illegal), not the whole rest of this loop: `1 == 2 && true`
     * must still parse as `(1 == 2) && true` (&&'s own looser precedence
     * is a perfectly valid continuation), and `1 < 2 == 3` must still
     * parse as `(1 < 2) == 3` (== and < are DIFFERENT precedences, so
     * neither blocks the other). Do NOT replace this with a plain
     * `break`: that over-blocks both of those cases, caught by the
     * project's own test-e2e suite (ops_precedence_add_eq_and.nix)
     * failing with a parse error on the first one. */
    int last_non_assoc_prec = -1;
    for (;;) {
        const BinOpInfo *info = lookup_binop(cur(p).kind);
        if (!info || info->prec < min_prec) break;
        if (info->non_assoc && info->prec == last_non_assoc_prec) break;
        advance(p);
        int next_min_prec = info->right_assoc ? info->prec : info->prec + 1;
        Ast *rhs = parse_binop(p, next_min_prec);
        lhs = make_binop_node(info, lhs, rhs);
        if (info->non_assoc) last_non_assoc_prec = info->prec;
    }
    return lhs;
}

static Ast *parse_impl(Parser *p) {
    return parse_binop(p, 0);
}

/* Parses the bindings following `let` into one flat binding list (real Nix
 * `let ... in` is always implicitly `letrec`: every binding sees itself
 * and every sibling regardless of source order -- see ast.h/compiler.c). */
static Ast *parse_let_bindings(Parser *p) {
    BTVec bt = { 0 };
    while (cur(p).kind != TOK_IN) parse_binding_stmt(p, &bt, NULL); /* NULL: reject dynamic attrs */
    advance(p); /* 'in' */
    Ast *body = parse_expr(p);

    /* `let` has nowhere to put a dynamic binding absorbed via
     * reopen_attrset_into (AST_LET has no dyn_bindings field at all,
     * unlike AST_ATTRSET) -- fatal here with the same message a direct
     * top-level `let ${k} = v;` already gets (parse_binding_stmt's own
     * `dyn_out == NULL` check), rather than silently dropping it. Not
     * believed reachable today, kept purely as a loud-not-silent guard. */
    if (bt.dyn.len > 0) {
        fprintf(stderr, "ll: parse error: dynamic attributes not allowed in let\n");
        exit(1);
    }
    BindingVec bindings = { 0 };
    /* Lazy-thunk-compile experiment, ported from asnix: same reasoning
     * as TOK_LBRACE/TOK_REC's own identical wrap in parse_atom --
     * finalize_bt_vec is where any attrpath-sugar's synthetic
     * AST_ATTRSET wrapper node actually gets allocated, strictly AFTER
     * the while-loop above has already returned, so the per-binding
     * toggle inside parse_binding_stmt cannot reach it on its own. */
    int lt_prev_let_finalize = ll_ast_enter_retained();
    finalize_bt_vec(&bt, &bindings);
    ll_ast_leave_retained(lt_prev_let_finalize);
    VEC_TRIM(bindings, AstBinding);

    Ast *n = ll_ast_new_at(AST_LET, cur(p).start);
    n->as.let1.bindings = bindings.items;
    n->as.let1.count = bindings.len;
    n->as.let1.body = body;
    return n;
}

/* Decides whether a TOK_LBRACE opens a lambda-pattern formals list
 * (`{...}: body` / `{...}@name: body`) or an ordinary attrset-literal
 * expression, using only the next 1-2 tokens -- never scanning ahead to
 * the matching '}'. A lambda pattern's own grammar is fully determined
 * by its first one or two tokens --
 *   `{ ...`                    -- ellipsis-only or ellipsis-leading formals
 *   `{ }` followed by ':'/'@'  -- empty formals
 *   `{ IDENT` followed by ','/'?'/'}' -- a formals entry (name[, ...] /
 *                                        name?default / the sole formal)
 * -- anything else is an attrset literal (a bare identifier followed by
 * '=' or '.', a string/dynamic key, `inherit`, etc., none of which share
 * a prefix with a valid formals list). `p` is always positioned with
 * `cur(p)` exactly AT the `{` itself, so `t1` is just `peek2(p)`; the one
 * occasional third token of lookahead (`t2`) goes through `peek3()`
 * (parser.c's own save/probe/rewind helper, see its comment). */
static int is_formals_pattern(Parser *p) {
    TokKind t1 = peek2(p).kind;
    if (t1 == TOK_ELLIPSIS) return 1;
    if (t1 == TOK_RBRACE) {
        TokKind t2 = peek3(p);
        return t2 == TOK_COLON || t2 == TOK_AT;
    }
    if (t1 == TOK_IDENT) {
        TokKind t2 = peek3(p);
        return t2 == TOK_COMMA || t2 == TOK_QUESTION || t2 == TOK_RBRACE;
    }
    return 0;
}

/* Parses the comma-separated formals list of a lambda pattern, with the
 * parser positioned just after the opening '{'. Consumes through (and
 * including) the matching '}'. Trailing commas are accepted (including
 * right before `...`), matching real Nix. */
static void parse_formals(Parser *p, AstFormal **out_items, size_t *out_count, int *out_ellipsis) {
    size_t formals_mark = formalvec_mark();
    int ellipsis = 0;
    while (cur(p).kind != TOK_RBRACE) {
        if (cur(p).kind == TOK_ELLIPSIS) {
            advance(p);
            ellipsis = 1;
            break; /* `...` must be the last formal */
        }
        Token name = expect(p, TOK_IDENT, "a formal name");
        Ast *default_ = NULL;
        if (cur(p).kind == TOK_QUESTION) {
            advance(p);
            default_ = parse_expr(p); /* arbitrary lazy expression -- see compiler.c's AST_LAMBDA case */
        }
        formalvec_push((AstFormal){ .name = tok_text(p, name), .name_len = tok_len(name), .default_ = default_ });
        if (cur(p).kind == TOK_COMMA) {
            advance(p);
            continue;
        }
        break;
    }
    expect(p, TOK_RBRACE, "'}'");
    *out_items = formalvec_commit(formals_mark, out_count);
    *out_ellipsis = ellipsis;
}

static Ast *parse_expr(Parser *p) {
    if (cur(p).kind == TOK_LET) {
        advance(p);
        if (cur(p).kind == TOK_LBRACE) {
            /* Legacy `let { ..., body = ...; }` form -- real Nix desugars
             * this directly to `(rec { ... }).body` (cppnix's parser.y:
             * "Let expressions `let {..., body = ...}' are just desugared
             * into `(rec {..., body = ...}).body'."). Distinct from
             * ordinary `let <bindings> in <expr>` (no `{` immediately
             * after `let`) handled below. */
            advance(p); /* '{' */
            BTVec bt = { 0 };
            DynBindingVec dyn = { 0 };
            int lt_prev_letbrace = ll_ast_enter_retained(); /* see TOK_LBRACE's own comment above */
            while (cur(p).kind != TOK_RBRACE) parse_binding_stmt(p, &bt, &dyn);
            advance(p); /* '}' */
            BindingVec bindings = { 0 };
            finalize_bt_vec(&bt, &bindings);
            VEC_TRIM(bindings, AstBinding);
            Ast *attrs = ll_ast_new_at(AST_ATTRSET, cur(p).start);
            attrs->as.attrset.bindings = bindings.items;
            attrs->as.attrset.count = bindings.len;
            attrs->as.attrset.is_rec = 1;
            merge_bt_dyn_into(&bt, &dyn);
            attrs->as.attrset.dyn_bindings = finalize_dyn_vec(&dyn);
            attrs->as.attrset.dyn_count = dyn.len;
            ll_ast_leave_retained(lt_prev_letbrace);
            Ast *n = ll_ast_new_at(AST_SELECT, cur(p).start);
            n->as.select.target = attrs;
            n->as.select.attr = "body";
            n->as.select.attr_len = 4;
            n->as.select.attr_expr = NULL;
            n->as.select.default_ = NULL;
            return n;
        }
        return parse_let_bindings(p);
    }
    if (cur(p).kind == TOK_WITH) {
        advance(p);
        Ast *attrs = parse_expr(p);
        expect(p, TOK_SEMI, "';'");
        Ast *body = parse_expr(p); /* recursing here is what lets further let/if/with/lambda follow */
        Ast *n = ll_ast_new_at(AST_WITH, cur(p).start);
        n->as.with1.attrs = attrs;
        n->as.with1.body = body;
        return n;
    }
    if (cur(p).kind == TOK_IF) {
        advance(p);
        Ast *cond = parse_expr(p);
        expect(p, TOK_THEN, "'then'");
        Ast *then_ = parse_expr(p);
        expect(p, TOK_ELSE, "'else'");
        Ast *else_ = parse_expr(p);
        Ast *n = ll_ast_new_at(AST_IF, cur(p).start);
        n->as.if1.cond = cond;
        n->as.if1.then_ = then_;
        n->as.if1.else_ = else_;
        return n;
    }
    if (cur(p).kind == TOK_IDENT && peek2(p).kind == TOK_COLON) {
        Token param = cur(p);
        advance(p);
        advance(p); /* ':' */
        Ast *body = parse_expr(p); /* recursing here is what makes `a: b: body` curry */
        Ast *n = ll_ast_new_at(AST_LAMBDA, cur(p).start);
        n->as.lambda.param = tok_text(p, param);
        n->as.lambda.param_len = tok_len(param);
        n->as.lambda.body = body;
        return n;
    }
    if (cur(p).kind == TOK_ASSERT) {
        advance(p);
        Token start_tok = cur(p);
        Ast *cond = parse_expr(p);
        Token semi_tok = cur(p); /* captured before expect() consumes it */
        expect(p, TOK_SEMI, "';'");
        size_t span_len = (size_t)(semi_tok.start - start_tok.start);
        while (span_len > 0 && isspace((unsigned char)p->src[start_tok.start + span_len - 1])) span_len--;
        Ast *body = parse_expr(p);
        Ast *n = ll_ast_new_at(AST_ASSERT, cur(p).start);
        n->as.assert1.cond = cond;
        n->as.assert1.body = body;
        n->as.assert1.cond_text = p->src + start_tok.start;
        n->as.assert1.cond_text_len = span_len;
        return n;
    }
    if (cur(p).kind == TOK_IDENT && peek2(p).kind == TOK_AT) {
        /* `name@{...}: body` -- the whole-arg alias comes first. */
        Token asname = cur(p);
        advance(p);
        advance(p); /* '@' */
        expect(p, TOK_LBRACE, "'{' after '@'");
        AstFormal *formals;
        size_t fcount;
        int ellipsis;
        parse_formals(p, &formals, &fcount, &ellipsis);
        expect(p, TOK_COLON, "':'");
        Ast *body = parse_expr(p);
        Ast *n = ll_ast_new_at(AST_LAMBDA, cur(p).start);
        n->as.lambda.is_pattern = 1;
        n->as.lambda.formals = formals;
        n->as.lambda.formals_count = fcount;
        n->as.lambda.has_ellipsis = ellipsis;
        n->as.lambda.has_as_name = 1;
        n->as.lambda.as_name = tok_text(p, asname);
        n->as.lambda.as_name_len = tok_len(asname);
        n->as.lambda.body = body;
        return n;
    }
    if (cur(p).kind == TOK_LBRACE) {
        /* Could be a lambda pattern (`{...}: body` / `{...}@name: body`) or
         * an ordinary attrset-literal expression -- a bounded lookahead at
         * the first 1-2 tokens tells which, same disambiguation real Nix's
         * own grammar makes (see is_formals_pattern's comment). */
        if (is_formals_pattern(p)) {
            advance(p); /* '{' */
            AstFormal *formals;
            size_t fcount;
            int ellipsis;
            parse_formals(p, &formals, &fcount, &ellipsis);
            int has_as = 0;
            const char *as_name = NULL;
            size_t as_name_len = 0;
            if (cur(p).kind == TOK_AT) {
                advance(p);
                Token asname = expect(p, TOK_IDENT, "a name after '@'");
                has_as = 1;
                as_name = tok_text(p, asname);
                as_name_len = tok_len(asname);
            }
            expect(p, TOK_COLON, "':'");
            Ast *body = parse_expr(p);
            Ast *n = ll_ast_new_at(AST_LAMBDA, cur(p).start);
            n->as.lambda.is_pattern = 1;
            n->as.lambda.formals = formals;
            n->as.lambda.formals_count = fcount;
            n->as.lambda.has_ellipsis = ellipsis;
            n->as.lambda.has_as_name = has_as;
            n->as.lambda.as_name = as_name;
            n->as.lambda.as_name_len = as_name_len;
            n->as.lambda.body = body;
            return n;
        }
        /* Not a pattern: fall through to parse_impl (the top of the full
         * operator-precedence chain), whose own fallthrough eventually
         * reaches parse_atom, which handles '{' as an attrset literal. */
    }
    return parse_impl(p);
}

/* Fuses lexing and parsing -- src is lexed on demand through a Lexer, a
 * true pull cursor (lexer.h) with no backing Token array at all.
 * `p.tok`/`p.next` are primed here by calling lexer_next() twice up
 * front, so `peek2()` is always valid even right at TOK_EOF (lexing an
 * empty file primes both slots to TOK_EOF). */
Ast *ll_parse(const char *src) {
    Lexer *lx = lexer_new(src);
    Parser p = { .lx = lx, .src = src, .curpos_base = src };
    p.tok = lexer_next(lx);
    p.next = lexer_next(lx);
    Ast *result = parse_expr(&p);
    expect(&p, TOK_EOF, "end of input");
    lexer_free(lx);
    return result;
}

/* Marker-reparse addition (bytecode-cache + lazy-thunk reconciliation --
 * see compiler.c's LazyThunkDescriptor comment and bytecode.c's
 * PTAG_OP_LAZY_MARKER comment): parses exactly ONE expression starting
 * at byte offset `start_offset` within `src`, which is the SAME full,
 * NUL-terminated file buffer an ordinary ll_parse(src) call would
 * have used (never a fresh copy) -- `start_offset` always lands right
 * after a `name = ` (or attrpath-sugar `a.b.c = `) token sequence, at
 * the first token of that binding's RHS (see parser.c's own
 * parse_binding_stmt_inner, which is the only place that ever stamps a
 * value Ast's `src_pos` with a real, reparseable span).
 *
 * Deliberately does NOT call `expect(&p, TOK_EOF, ...)` the way
 * ll_parse does -- unlike a whole top-level file, there is always
 * real, meaningful content after this one expression ends (at minimum
 * the `;` that closed the original binding statement, usually a lot
 * more: sibling bindings, the enclosing `}`, everything after it). This
 * is exactly why no explicit end-of-range length is needed either: the
 * Lexer here is a pure forward-scanning cursor over `src` with no
 * dependency on what precedes its own starting point (confirmed:
 * lexer_new just stores a `const char *`/`pos` pair, see lexer.c), so
 * pointing it at `src + start_offset` is exactly as safe as pointing it
 * at `src + 0` (ll_parse's own case) -- `parse_expr` naturally stops
 * consuming tokens the instant this one expression is grammatically
 * complete (recursive descent, not a greedy consume-until-EOF loop), and
 * this function simply never looks at (or lexes past what it needs to
 * confirm the expression ended -- at most one token of lookahead via
 * `p.next`) whatever real content follows. `src`'s true start (not
 * `start_offset`) is passed as `curpos_base` so a `__curPos` inside the
 * reparsed expression still reports its real, absolute line/column
 * (see Parser's own `curpos_base` field comment) -- `__curPos`'s own
 * `file` field (ast.h's AST_CURPOS comment) is set later, by
 * compile_expr, from the current `g_compile_file_path` global, same as
 * any other compile of this same body would set it, so that part needs
 * no special handling here at all. */
Ast *ll_parse_subexpr(const char *src, size_t start_offset) {
    /* lexer_new_lenient, NOT lexer_new -- see that function's own
     * comment. Found the hard way: forcing a cache-loaded deferred
     * field whose value is the LAST binding before its enclosing `}`
     * (e.g. `rec { a = 1; b = a + 1; }`, forcing `b`) crashed with
     * "ll: unmatched '}'" -- the ordinary lexer_new's ONE token of
     * required lookahead past this expression's own end (`cur()`/
     * `peek2()`'s 2-slot buffer, primed below) reached the enclosing
     * rec{}'s own closing brace, which THIS fresh Lexer, starting mid-
     * file, never recorded the matching `{` for. */
    Lexer *lx = lexer_new_lenient(src + start_offset);
    Parser p = { .lx = lx, .src = src + start_offset, .curpos_base = src };
    p.tok = lexer_next(lx);
    p.next = lexer_next(lx);
    Ast *result = parse_expr(&p);
    lexer_free(lx);
    return result;
}
