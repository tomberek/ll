/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
/*
 * A small, hand-written POSIX ERE backtracking matcher -- see
 * docs/architecture.md's M68 scope notes for the full story. Short
 * version: `builtins.match`/`builtins.split` (builtins_regex.c) were
 * wired onto glibc's <regex.h>, but real Nix's `primops.cc` uses
 * libstdc++'s `std::regex` (built with `std::regex::extended`), and the
 * two engines disagree on two specific things glibc itself cannot be
 * talked out of:
 *
 *  - Which capture a repeated/optional subexpression gets when the same
 *    overall match is reachable via more than one decomposition (e.g.
 *    `(a*)*` against "aa": glibc's regexec reports group 1 as "aa",
 *    libstdc++ reports "" -- confirmed directly against both engines,
 *    see M55's scope notes). This is NOT about *whether* a match
 *    exists, or how long it is -- glibc's own leftmost-longest
 *    whole-match-finding is correct and unaffected; only the
 *    subexpression bookkeeping differs.
 *  - A handful of POSIX bracket-expression extensions: multi-character
 *    named collating symbols (`[.hyphen.]`), equivalence classes
 *    (`[=a=]`, which needs case-fold semantics glibc's own matching
 *    doesn't apply), and non-lowercase/non-standard character class
 *    names (`[:ALPHA:]`, the GNU `[:w:]` extension) -- glibc's regcomp
 *    rejects most of these outright (REG_ECOLLATE/REG_ECTYPE), and gets
 *    the one it does accept syntactically (`[=a=]`) semantically wrong.
 *
 * Rather than trying to characterize and patch around libstdc++'s
 * specific backtracking quirks from the outside, this file is a
 * complete (if deliberately narrow -- just the ERE subset
 * nix-conformance's regex corpus and nixpkgs actually exercise: `. ^ $
 * * + ? {m,n} ( ) |` and bracket expressions, no lookaround/backrefs/
 * lazy quantifiers, none of which ERE has anyway) backtracking regex
 * engine built to reproduce libstdc++'s specific algorithm: ordered
 * alternation (try branches in the order they're written, first one
 * whose *entire remaining continuation* eventually succeeds wins -- not
 * "prefer the longest branch", a distinction confirmed directly: real
 * Nix's `(aa|a)(aa|a)` on "aaa" gives `[ "aa" "a" ]`, i.e. the
 * first-listed "aa" branch is taken simply because it's tried first and
 * happens to work, while `(a|aa)(a|aa)` on the same string gives
 * `[ "a" "aa" ]` -- first-listed "a" tried first, also happens to work;
 * neither result has anything to do with which branch is longer) plus
 * greedy repetition with one specific, well-known backtracking-engine
 * quirk: a `*`/`+`/`{m,}` loop, once its real (non-empty) iterations are
 * exhausted, still greedily attempts exactly one more iteration even if
 * the loop body can only match the empty string there, and -- if that
 * empty attempt is itself ultimately consistent with the rest of the
 * pattern matching -- its capture overwrites whatever the last
 * non-empty iteration had recorded, before the loop gives up for good
 * (confirmed directly: `(a*)*` on "aa" -> `[ "" ]`, not `[ "aa" ]`; the
 * much subtler `((a)*)*` on "aa" -> `[ "" "a" ]`, confirming group
 * state is untouched -- not reset to null, not overwritten -- whenever
 * an enclosing loop iteration runs its OWN body zero times, as opposed
 * to running it once with an empty result; see `match_rep` below for
 * where this is implemented and the exact mechanism).
 *
 * Architecture: parse to a small AST (`RNode`), then match via
 * continuation-passing-style backtracking (`m`/`run_cont`/`match_rep`
 * below) -- the textbook approach for POSIX/ERE-shaped backtracking
 * matchers (see e.g. Russ Cox's "Regular Expression Matching: the
 * Virtual Machine Approach" for the general family; this is the
 * simplest member of it, recursive-descent-over-the-AST rather than a
 * compiled bytecode VM, since none of this project's regex inputs are
 * large/adversarial enough to need the latter's guarantees -- see the
 * "performance" paragraph in M68's scope notes for the actual numbers
 * checked). Every AST node's `m()` case calls into its continuation `k`
 * (a tiny linked stack of "what to try matching next", built on the C
 * call stack as ordinary recursion -- no heap allocation in the match
 * path at all) exactly once per candidate way of matching itself, in
 * priority order, and returns as soon as one such call succeeds -- that
 * "stop at first success" is what makes alternation/quantifier choice
 * ORDERED instead of exhaustive. The one place this file deliberately
 * does NOT stop at first success is top-level search mode (`anchored ==
 * 0`, used only for `asnix_regex_search`'s from-scratch length-finding
 * pass): there, the terminal continuation always reports failure (to
 * force every reachable leaf to be visited) while recording the
 * longest position actually reached, which is how leftmost-longest
 * whole-match-finding is reproduced without glibc's help for the
 * handful of patterns that need that (see `asnix_regex_needs_custom_engine`).
 *
 * Builtins_regex.c decides, per call, whether any of this is needed at
 * all: a pattern with zero capture groups and none of the above
 * bracket-expression extensions never touches this file -- glibc's
 * regexec is already both correct and fast for that case, and stays the
 * only engine invoked.
 */
#include "regex_engine.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------
 * AST
 * --------------------------------------------------------------------- */

typedef enum {
    RN_LIT,   /* single literal byte */
    RN_ANY,   /* '.' */
    RN_CLASS, /* bracket expression -- 256-byte membership table */
    RN_BOL,   /* '^' */
    RN_EOL,   /* '$' */
    RN_SEQ,   /* concatenation */
    RN_ALT,   /* alternation */
    RN_GROUP, /* capturing group */
    RN_REP    /* *, +, ?, {m,n} -- uniformly min/max repetitions of child */
} RNKind;

typedef struct RNode RNode;
struct RNode {
    RNKind kind;
    char lit;              /* RN_LIT */
    unsigned char *cls;    /* RN_CLASS: cls[byte] != 0 means byte is a member */
    RNode **kids;          /* RN_SEQ / RN_ALT */
    int nkids;
    RNode *child;          /* RN_GROUP / RN_REP */
    int group_idx;         /* RN_GROUP: 1-based capture index */
    int rep_min, rep_max;  /* RN_REP: max == -1 means unbounded */
};

struct AsnixCustomRegex {
    RNode *root;
    int64_t nsub;
};

static RNode *new_node(RNKind k) {
    RNode *n = calloc(1, sizeof *n);
    n->kind = k;
    return n;
}

static void free_node(RNode *n) {
    if (!n) return;
    for (int i = 0; i < n->nkids; i++) free_node(n->kids[i]);
    free(n->kids);
    free_node(n->child);
    free(n->cls);
    free(n);
}

/* ---------------------------------------------------------------------
 * Parser -- recursive descent over the ERE grammar subset documented
 * in this file's header comment. Every parse_* function returns NULL
 * and leaves ps->error set on failure; callers propagate that
 * immediately. Group indices are assigned left-to-right as '(' is
 * encountered, matching regcomp's own re_nsub numbering.
 * --------------------------------------------------------------------- */

typedef struct {
    const char *p;
    int64_t pos, len;
    int next_group;
    int error;
} Parser;

static int pk(const Parser *ps) { return ps->pos < ps->len ? (unsigned char)ps->p[ps->pos] : -1; }
static int pk2(const Parser *ps) { return ps->pos + 1 < ps->len ? (unsigned char)ps->p[ps->pos + 1] : -1; }

/* Named collating symbol lookup (`[.name.]`): a single-character name is
 * that character -- but ONLY when it's an ASCII letter. Confirmed
 * directly against real Nix/libstdc++: `[[.a.]]`/`[[.Z.]]` are accepted,
 * but `[[.-.]]`/`[[.1.]]`/`[[._.]]`/`[[. .]]` (any non-alphabetic single
 * byte) are all rejected ("Invalid collate element") -- libstdc++'s
 * collating-element table only has single-character entries for letters;
 * every other single byte needs one of the POSIX portable character
 * set's multi-character symbolic names below (e.g. `[.hyphen.]`, not
 * `[.-.]`) instead. Letters/digits-as-multi-char-names don't need table
 * entries since digits fail the single-character rule and have no
 * standard multi-char name either (and indeed `[[.digit...]]`-shaped
 * names aren't in the POSIX list below). */
static int collate_lookup(const char *name, int64_t namelen, unsigned char *out) {
    if (namelen == 1) {
        if (!isalpha((unsigned char)name[0])) return 0;
        *out = (unsigned char)name[0];
        return 1;
    }
    static const struct { const char *name; unsigned char ch; } table[] = {
        { "NUL", 0 }, { "alert", '\a' }, { "backspace", '\b' }, { "tab", '\t' },
        { "newline", '\n' }, { "vertical-tab", '\v' }, { "form-feed", '\f' },
        { "carriage-return", '\r' }, { "space", ' ' },
        { "exclamation-mark", '!' }, { "quotation-mark", '"' }, { "number-sign", '#' },
        { "dollar-sign", '$' }, { "percent-sign", '%' }, { "ampersand", '&' },
        { "apostrophe", '\'' }, { "left-parenthesis", '(' }, { "right-parenthesis", ')' },
        { "asterisk", '*' }, { "plus-sign", '+' }, { "comma", ',' },
        { "hyphen", '-' }, { "hyphen-minus", '-' }, { "period", '.' }, { "full-stop", '.' },
        { "slash", '/' }, { "solidus", '/' }, { "colon", ':' }, { "semicolon", ';' },
        { "less-than-sign", '<' }, { "equals-sign", '=' }, { "greater-than-sign", '>' },
        { "question-mark", '?' }, { "commercial-at", '@' },
        { "left-square-bracket", '[' }, { "backslash", '\\' }, { "reverse-solidus", '\\' },
        { "right-square-bracket", ']' }, { "circumflex", '^' }, { "circumflex-accent", '^' },
        { "underscore", '_' }, { "low-line", '_' }, { "grave-accent", '`' },
        { "left-brace", '{' }, { "left-curly-bracket", '{' }, { "vertical-line", '|' },
        { "right-brace", '}' }, { "right-curly-bracket", '}' }, { "tilde", '~' },
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        if ((int64_t)strlen(table[i].name) == namelen && memcmp(table[i].name, name, (size_t)namelen) == 0) {
            *out = table[i].ch;
            return 1;
        }
    }
    return 0;
}

/* POSIX character class test, case-insensitive on the class name (real
 * Nix/libstdc++ accepts `[:Alpha:]`/`[:ALPHA:]` etc, confirmed directly
 * -- see M68's scope notes), plus the GNU `[:w:]` ("word": alnum or
 * underscore) extension, also confirmed directly against real Nix.
 * `*recognized` reports whether `name` was a known class at all
 * (distinct from the per-byte true/false test), so callers can reject
 * truly unknown names (`[:foo:]`) instead of silently matching nothing. */
static int class_test(const char *name, int64_t namelen, unsigned char ch, int *recognized) {
    char lname[16];
    *recognized = 0;
    if (namelen <= 0 || namelen >= (int64_t)sizeof lname) return 0;
    for (int64_t i = 0; i < namelen; i++) lname[i] = (char)tolower((unsigned char)name[i]);
    lname[namelen] = '\0';
    *recognized = 1;
    if (!strcmp(lname, "alpha")) return isalpha(ch) != 0;
    if (!strcmp(lname, "digit")) return isdigit(ch) != 0;
    if (!strcmp(lname, "alnum")) return isalnum(ch) != 0;
    if (!strcmp(lname, "upper")) return isupper(ch) != 0;
    if (!strcmp(lname, "lower")) return islower(ch) != 0;
    if (!strcmp(lname, "space")) return isspace(ch) != 0;
    if (!strcmp(lname, "blank")) return ch == ' ' || ch == '\t';
    if (!strcmp(lname, "cntrl")) return iscntrl(ch) != 0;
    if (!strcmp(lname, "punct")) return ispunct(ch) != 0;
    if (!strcmp(lname, "print")) return isprint(ch) != 0;
    if (!strcmp(lname, "graph")) return isgraph(ch) != 0;
    if (!strcmp(lname, "xdigit")) return isxdigit(ch) != 0;
    if (!strcmp(lname, "w")) return isalnum(ch) != 0 || ch == '_';
    *recognized = 0;
    return 0;
}

/* This function is only ever called for a range's HIGH endpoint (the
 * LOW endpoint -- plain byte or `[.name.]` collating symbol -- is parsed
 * directly in `parse_bracket` below). A named collating symbol
 * (`[.name.]`) is valid as a range's LOW endpoint (`[[.a.]-c]` --
 * confirmed directly against real Nix, see M68's scope notes) but is
 * NEVER valid as its HIGH endpoint, regardless of what the low endpoint
 * was: confirmed directly against real Nix/libstdc++, both
 * `[a-[.c.]]` and `[[.a.]-[.c.]]` are rejected ("Invalid end of '[x-x]'
 * range") while `[[.a.]-c]` (collating symbol as LOW, plain byte as
 * HIGH) is accepted -- so reject the `[.name.]` form outright here
 * rather than parsing and discarding it. */
static int parse_range_endpoint(Parser *ps, unsigned char *out) {
    if (pk(ps) == '[' && pk2(ps) == '.') return 0;
    if (pk(ps) < 0) return 0;
    *out = (unsigned char)pk(ps);
    ps->pos++;
    return 1;
}

static RNode *parse_bracket(Parser *ps) {
    /* ps->p[ps->pos] == '[' on entry */
    ps->pos++;
    int negate = 0;
    if (pk(ps) == '^') {
        negate = 1;
        ps->pos++;
    }
    unsigned char *cls = calloc(256, 1);
    int first = 1;
    for (;;) {
        int c = pk(ps);
        if (c < 0) { ps->error = 1; free(cls); return NULL; }
        if (c == ']' && !first) break;
        first = 0;
        if (c == '[' && pk2(ps) == '.') {
            int64_t j = ps->pos + 2;
            while (j + 1 < ps->len && !(ps->p[j] == '.' && ps->p[j + 1] == ']')) j++;
            if (j + 1 >= ps->len) { ps->error = 1; free(cls); return NULL; }
            int64_t namestart = ps->pos + 2, namelen = j - namestart;
            unsigned char ch;
            if (!collate_lookup(ps->p + namestart, namelen, &ch)) { ps->error = 1; free(cls); return NULL; }
            ps->pos = j + 2;
            if (pk(ps) == '-' && pk2(ps) != ']' && pk2(ps) >= 0) {
                ps->pos++;
                unsigned char hi;
                if (!parse_range_endpoint(ps, &hi)) { ps->error = 1; free(cls); return NULL; }
                if (hi < ch) { ps->error = 1; free(cls); return NULL; }
                for (int v = ch; v <= hi; v++) cls[v] = 1;
            } else {
                cls[ch] = 1;
            }
            continue;
        }
        if (c == '[' && pk2(ps) == ':') {
            int64_t j = ps->pos + 2;
            while (j + 1 < ps->len && !(ps->p[j] == ':' && ps->p[j + 1] == ']')) j++;
            if (j + 1 >= ps->len) { ps->error = 1; free(cls); return NULL; }
            int64_t namestart = ps->pos + 2, namelen = j - namestart;
            /* `recognized` doesn't vary with the byte being tested (it's
             * purely a function of the class name), so one probe call
             * tells us whether the name itself is known at all; the loop
             * below fills in `cls[]` for every byte either way. */
            int recognized;
            class_test(ps->p + namestart, namelen, 'a', &recognized);
            if (!recognized) { ps->error = 1; free(cls); return NULL; }
            for (int v = 0; v < 256; v++) {
                int rec;
                if (class_test(ps->p + namestart, namelen, (unsigned char)v, &rec)) cls[v] = 1;
            }
            ps->pos = j + 2;
            continue;
        }
        if (c == '[' && pk2(ps) == '=') {
            int64_t j = ps->pos + 2;
            while (j + 1 < ps->len && !(ps->p[j] == '=' && ps->p[j + 1] == ']')) j++;
            if (j + 1 >= ps->len) { ps->error = 1; free(cls); return NULL; }
            int64_t namestart = ps->pos + 2, namelen = j - namestart;
            unsigned char ch;
            if (!collate_lookup(ps->p + namestart, namelen, &ch)) { ps->error = 1; free(cls); return NULL; }
            cls[ch] = 1;
            if (isalpha(ch)) cls[(unsigned char)(islower(ch) ? toupper(ch) : tolower(ch))] = 1;
            ps->pos = j + 2;
            /* Unlike a named collating symbol (`[[.a.]-c]`, confirmed
             * valid as a range endpoint -- see parse_range_endpoint),
             * an equivalence class can't be one: confirmed directly
             * against real Nix, `[[=a=]-c]` is rejected
             * (nix-conformance's regex-invalid corpus entry 41) even
             * though this engine's own bracket grammar would otherwise
             * parse the trailing `-c` as ordinary literal members with
             * no error. */
            if (pk(ps) == '-' && pk2(ps) != ']' && pk2(ps) >= 0) { ps->error = 1; free(cls); return NULL; }
            continue;
        }
        unsigned char ch = (unsigned char)c;
        ps->pos++;
        if (pk(ps) == '-' && pk2(ps) != ']' && pk2(ps) >= 0) {
            ps->pos++;
            unsigned char hi;
            if (!parse_range_endpoint(ps, &hi)) { ps->error = 1; free(cls); return NULL; }
            if (hi < ch) { ps->error = 1; free(cls); return NULL; }
            for (int v = ch; v <= hi; v++) cls[v] = 1;
        } else {
            cls[ch] = 1;
        }
    }
    ps->pos++; /* closing ']' */
    if (negate) for (int v = 0; v < 256; v++) cls[v] = !cls[v];
    RNode *n = new_node(RN_CLASS);
    n->cls = cls;
    return n;
}

static int parse_digits(Parser *ps, int *out) {
    int64_t start = ps->pos;
    int v = 0, any = 0;
    while (pk(ps) >= 0 && isdigit(pk(ps))) {
        v = v * 10 + (pk(ps) - '0');
        if (v > 100000) v = 100000; /* clamp -- same spirit as glibc's own RE_DUP_MAX cap */
        ps->pos++;
        any = 1;
    }
    if (!any) { ps->pos = start; return 0; }
    *out = v;
    return 1;
}

/* `{m}` / `{m,}` / `{m,n}` -- ps->p[ps->pos] == '{' on entry. Returns 0
 * (restoring ps->pos via the caller) rather than setting ps->error if
 * this doesn't look like a valid interval at all, so a bare `{` that
 * isn't one (e.g. "a}"'s stray '}' alone, or any other odd-but-valid
 * literal brace) falls through to being an ordinary literal character
 * instead of a hard parse failure. */
static int parse_interval(Parser *ps, int *min_out, int *max_out) {
    ps->pos++; /* '{' */
    int lo;
    if (!parse_digits(ps, &lo)) return 0;
    int hi;
    if (pk(ps) == ',') {
        ps->pos++;
        if (pk(ps) == '}') {
            hi = -1;
        } else if (!parse_digits(ps, &hi)) {
            return 0;
        }
    } else {
        hi = lo;
    }
    if (pk(ps) != '}') return 0;
    ps->pos++;
    *min_out = lo;
    *max_out = hi;
    return 1;
}

static RNode *parse_alt(Parser *ps);

static RNode *parse_atom(Parser *ps) {
    int c = pk(ps);
    if (c < 0) { ps->error = 1; return NULL; }
    if (c == '(') {
        ps->pos++;
        int gidx = ps->next_group++;
        RNode *inner = parse_alt(ps);
        if (!inner) return NULL;
        if (pk(ps) != ')') { ps->error = 1; free_node(inner); return NULL; }
        ps->pos++;
        RNode *g = new_node(RN_GROUP);
        g->child = inner;
        g->group_idx = gidx;
        return g;
    }
    if (c == '.') { ps->pos++; return new_node(RN_ANY); }
    if (c == '^') { ps->pos++; return new_node(RN_BOL); }
    if (c == '$') { ps->pos++; return new_node(RN_EOL); }
    if (c == '[') return parse_bracket(ps);
    if (c == '\\') {
        ps->pos++;
        int e = pk(ps);
        if (e < 0) { ps->error = 1; return NULL; }
        ps->pos++;
        RNode *n = new_node(RN_LIT);
        n->lit = (char)e;
        return n;
    }
    /* Any other byte, including a stray '{', '}', ')' that didn't close
     * anything, etc: an ordinary literal. ')' and '|' never reach here
     * -- parse_seq/parse_alt stop before calling parse_atom on them. */
    ps->pos++;
    RNode *n = new_node(RN_LIT);
    n->lit = (char)c;
    return n;
}

static RNode *parse_piece(Parser *ps) {
    RNode *atom = parse_atom(ps);
    if (!atom) return NULL;
    for (;;) {
        int c = pk(ps);
        int lo, hi;
        if (c == '*') { lo = 0; hi = -1; ps->pos++; }
        else if (c == '+') { lo = 1; hi = -1; ps->pos++; }
        else if (c == '?') { lo = 0; hi = 1; ps->pos++; }
        else if (c == '{') {
            int64_t save = ps->pos;
            if (!parse_interval(ps, &lo, &hi)) { ps->pos = save; break; }
        } else {
            break;
        }
        RNode *rep = new_node(RN_REP);
        rep->child = atom;
        rep->rep_min = lo;
        rep->rep_max = hi;
        atom = rep; /* quantifiers stack: "a**"/"a{1,2}{2}" etc, confirmed valid */
    }
    return atom;
}

static RNode *parse_seq(Parser *ps) {
    RNode **kids = NULL;
    int n = 0, cap = 0;
    for (;;) {
        int c = pk(ps);
        if (c < 0 || c == '|' || c == ')') break;
        RNode *piece = parse_piece(ps);
        if (!piece) {
            for (int i = 0; i < n; i++) free_node(kids[i]);
            free(kids);
            return NULL;
        }
        if (n >= cap) {
            cap = cap ? cap * 2 : 4;
            kids = realloc(kids, sizeof(RNode *) * (size_t)cap);
        }
        kids[n++] = piece;
    }
    RNode *seq = new_node(RN_SEQ);
    seq->kids = kids;
    seq->nkids = n;
    return seq;
}

static RNode *parse_alt(Parser *ps) {
    RNode *first = parse_seq(ps);
    if (!first) return NULL;
    if (pk(ps) != '|') return first;
    RNode **kids = malloc(sizeof(RNode *) * 2);
    int n = 1;
    kids[0] = first;
    while (pk(ps) == '|') {
        ps->pos++;
        RNode *br = parse_seq(ps);
        if (!br) {
            for (int i = 0; i < n; i++) free_node(kids[i]);
            free(kids);
            return NULL;
        }
        kids = realloc(kids, sizeof(RNode *) * (size_t)(n + 1));
        kids[n++] = br;
    }
    RNode *alt = new_node(RN_ALT);
    alt->kids = kids;
    alt->nkids = n;
    return alt;
}

AsnixCustomRegex *asnix_regex_compile(const char *pattern, int64_t len, int64_t *nsub_out) {
    Parser ps = { pattern, 0, len, 1, 0 };
    RNode *root = parse_alt(&ps);
    if (!root || ps.error || ps.pos != ps.len) {
        free_node(root);
        return NULL;
    }
    AsnixCustomRegex *re = malloc(sizeof *re);
    re->root = root;
    re->nsub = ps.next_group - 1;
    if (nsub_out) *nsub_out = re->nsub;
    return re;
}

void asnix_regex_free(AsnixCustomRegex *re) {
    if (!re) return;
    free_node(re->root);
    free(re);
}

/* ---------------------------------------------------------------------
 * Matcher -- continuation-passing backtracking. See this file's header
 * comment for the overall design and the specific libstdc++ quirks this
 * is built to reproduce.
 * --------------------------------------------------------------------- */

typedef struct Cont Cont;
typedef enum { K_SEQREST, K_CLOSEGROUP, K_STARITER } KKind;
struct Cont {
    KKind kind;
    union {
        struct { RNode * const *kids; int n; int idx; } seqrest;
        struct { int idx; int64_t start; } closegroup;
        struct { const RNode *rep; int reps_done; int64_t start_pos; } stariter;
    } u;
    const Cont *next;
};

typedef struct {
    const char *subject;
    int64_t slen;
    int notbol;
    AsnixRegexGroup *groups; /* NULL if the caller doesn't want captures tracked */
    int64_t nsub;            /* number of entries in `groups` (0 if groups == NULL) */
    int anchored;            /* 1: match() mode, fixed required_end. 0: search mode (no fixed length). */
    int64_t required_end;    /* anchored mode only */
    int64_t best_end;        /* search mode only -- where the pattern ended, once found */
    int found;               /* search mode only */
    /* RN_ALT's search-mode "explore every branch" comparison (see its
     * own comment below) is what replicates libstdc++'s actual
     * leftmost-longest-among-alternatives behavior, but it is also
     * genuinely exponential when a repetition's own body is itself an
     * ambiguous alternation (e.g. `(a|aa)*b` over a long run of "a"s --
     * every iteration re-explores both branches across however many
     * iterations the REST of the string allows, multiplying out). None
     * of this project's actual target patterns (nix-conformance's
     * regex-NNN corpus, all single-digit-to-low-double-digit-character
     * inputs) come close to tripping this; nix-conformance's separate
     * "long inputs" corpus section does, deliberately, to stress
     * leftmost-longest whole-match-finding -- which glibc's regexec
     * already handled correctly before this milestone for exactly this
     * repeated-ambiguous-alternation shape (confirmed: it's only the
     * *unambiguous*-repetition-followed-by-something-optional shape,
     * e.g. `(x*)(xy)?`, where glibc and libstdc++'s choice of overall
     * match boundary itself actually differs -- see
     * builtins_regex.c's split2_impl routing comment). `*budget` is a
     * shared (pointer, so every recursive call and every ALT trial
     * copy decrements the same counter) step count, decremented once
     * per `m()` call; RN_ALT degrades from "explore every branch" to
     * plain accept-first-success once it runs out, bounding the total
     * work for any pattern/input instead of letting it blow up --
     * degraded-but-finite beats exponential, and for every pattern this
     * budget is actually sized for, exhaustion never happens at all. */
    int64_t *budget;
} MCtx;

static int m(const RNode *n, int64_t pos, const Cont *k, MCtx *ctx);
static int match_rep(const RNode *rep, int64_t pos, const Cont *k, MCtx *ctx, int reps_done, int blocked);

static int run_cont(const Cont *k, int64_t pos, MCtx *ctx) {
    if (!k) {
        if (ctx->anchored) return pos == ctx->required_end;
        /* Search mode: unlike match()'s fixed target, there's nothing to
         * compare `pos` against here -- reaching the end of the pattern
         * at all means "this specific (greedy) resolution just
         * completed", so accept it immediately (ordinary
         * accept-first-success backtracking, same as anchored mode, NOT
         * an exhaustive every-possible-length search -- see this file's
         * header comment on why RN_ALT, not this terminal, is where
         * libstdc++'s specific leftmost-longest-among-alternatives
         * behavior actually needs to be reproduced; confirmed directly
         * against real Nix that a bare greedy quantifier, e.g. `x*`
         * immediately followed by an optional `(xy)?`, does NOT
         * backtrack to fewer `x*` repetitions just because that would
         * let the optional group match more -- `split "(x*)(xy)?" "xy"`
         * commits to the greedy `x*` = "x" reading and never looks
         * back, even though `(x*)(xy)?` could otherwise match "xy" in
         * full). */
        ctx->found = 1;
        ctx->best_end = pos;
        return 1;
    }
    switch (k->kind) {
        case K_SEQREST:
            if (k->u.seqrest.idx >= k->u.seqrest.n) return run_cont(k->next, pos, ctx);
            {
                Cont nk;
                nk.kind = K_SEQREST;
                nk.u.seqrest.kids = k->u.seqrest.kids;
                nk.u.seqrest.n = k->u.seqrest.n;
                nk.u.seqrest.idx = k->u.seqrest.idx + 1;
                nk.next = k->next;
                return m(k->u.seqrest.kids[k->u.seqrest.idx], pos, &nk, ctx);
            }
        case K_CLOSEGROUP: {
            if (!ctx->groups) return run_cont(k->next, pos, ctx);
            int idx = k->u.closegroup.idx;
            AsnixRegexGroup save = ctx->groups[idx - 1];
            ctx->groups[idx - 1].so = k->u.closegroup.start;
            ctx->groups[idx - 1].eo = pos;
            int r = run_cont(k->next, pos, ctx);
            if (!r) ctx->groups[idx - 1] = save; /* backtrack: undo, as if this iteration never ran */
            return r;
        }
        case K_STARITER: {
            int64_t start_pos = k->u.stariter.start_pos;
            int blocked = (pos == start_pos); /* zero-width iteration: allow it once, never twice in a row */
            return match_rep(k->u.stariter.rep, pos, k->next, ctx, k->u.stariter.reps_done + 1, blocked);
        }
    }
    return 0;
}

static int m(const RNode *n, int64_t pos, const Cont *k, MCtx *ctx) {
    if (ctx->budget) (*ctx->budget)--;
    switch (n->kind) {
        case RN_LIT:
            if (pos < ctx->slen && ctx->subject[pos] == n->lit) return run_cont(k, pos + 1, ctx);
            return 0;
        case RN_ANY:
            if (pos < ctx->slen) return run_cont(k, pos + 1, ctx);
            return 0;
        case RN_CLASS:
            if (pos < ctx->slen && n->cls[(unsigned char)ctx->subject[pos]]) return run_cont(k, pos + 1, ctx);
            return 0;
        case RN_BOL:
            if (!ctx->notbol && pos == 0) return run_cont(k, pos, ctx);
            return 0;
        case RN_EOL:
            if (pos == ctx->slen) return run_cont(k, pos, ctx);
            return 0;
        case RN_SEQ:
            if (n->nkids == 0) return run_cont(k, pos, ctx);
            {
                Cont c;
                c.kind = K_SEQREST;
                c.u.seqrest.kids = n->kids;
                c.u.seqrest.n = n->nkids;
                c.u.seqrest.idx = 1;
                c.next = k;
                return m(n->kids[0], pos, &c, ctx);
            }
        case RN_ALT:
            if (ctx->anchored || n->nkids <= 1 || (ctx->budget && *ctx->budget <= 0)) {
                /* Anchored (match()) mode, trivial single-branch "ALT",
                 * or exploration budget exhausted (see MCtx's own
                 * comment on `budget` -- degrade to plain
                 * accept-first-success rather than keep exploring
                 * exponentially): ordinary accept-first-success
                 * backtracking already gives the right answer, because
                 * the fixed required_end at the terminal is the thing
                 * forcing exploration of a later branch when an earlier
                 * one can't reach it -- validated extensively (see
                 * regex_engine.c's own test battery) against real Nix's
                 * subexpression-capture quirks; no special-casing
                 * needed here. */
                for (int i = 0; i < n->nkids; i++)
                    if (m(n->kids[i], pos, k, ctx)) return 1;
                return 0;
            }
            /* Search mode (split's unanchored scan): genuinely explore
             * every branch to its own greedy completion (each branch's
             * own repetition/alternation choices still resolve via
             * ordinary accept-first-success -- only THIS node, a direct
             * multi-branch choice, gets compared across alternatives),
             * and keep whichever branch reaches furthest -- confirmed
             * directly against real Nix this is specifically an
             * alternation-level behavior, not a general
             * "leftmost-longest across every possible decomposition"
             * one: `split "a|ab" "xaby"` matches the longer "ab" (this
             * rule), but `split "(x*)(xy)?" "xy"` does NOT backtrack
             * `x*` down from its greedy "x" to the empty string even
             * though that would let the whole pattern match "xy" in
             * full (match_rep's own ordinary greedy-first-success rule,
             * unaffected by this ALT-specific exploration). Captures:
             * each trial runs against a snapshot-restored `groups`
             * array so one branch's (possibly-successful, later
             * discarded) attempt can't leak into another's; the winning
             * branch is then re-run for real once identified, to leave
             * `ctx->groups` holding exactly that branch's own effects
             * (cheap -- bounded by this pattern's own alternation count,
             * not by input length). */
            {
                AsnixRegexGroup *saved = NULL;
                if (ctx->groups && ctx->nsub > 0) {
                    saved = malloc((size_t)ctx->nsub * sizeof *saved);
                    memcpy(saved, ctx->groups, (size_t)ctx->nsub * sizeof *saved);
                }
                int64_t best_end = -1;
                int best_i = -1;
                for (int i = 0; i < n->nkids; i++) {
                    if (saved) memcpy(ctx->groups, saved, (size_t)ctx->nsub * sizeof *saved);
                    MCtx trial = *ctx;
                    trial.found = 0;
                    trial.best_end = -1;
                    if (m(n->kids[i], pos, k, &trial) && trial.best_end > best_end) {
                        best_end = trial.best_end;
                        best_i = i;
                    }
                }
                if (best_i < 0) {
                    if (saved) { memcpy(ctx->groups, saved, (size_t)ctx->nsub * sizeof *saved); free(saved); }
                    return 0;
                }
                if (saved) memcpy(ctx->groups, saved, (size_t)ctx->nsub * sizeof *saved);
                free(saved);
                return m(n->kids[best_i], pos, k, ctx); /* re-run for real: leaves ctx->groups/found/best_end correctly set */
            }
        case RN_GROUP: {
            Cont c;
            c.kind = K_CLOSEGROUP;
            c.u.closegroup.idx = n->group_idx;
            c.u.closegroup.start = pos;
            c.next = k;
            return m(n->child, pos, &c, ctx);
        }
        case RN_REP:
            return match_rep(n, pos, k, ctx, 0, 0);
    }
    return 0;
}

/* Repetition (`*`, `+`, `?`, `{m,n}`), uniformly: `reps_done` reps of
 * `rep->child` have already run; mandatory reps (reps_done < rep_min)
 * are forced regardless of `blocked` (there is no "stop early" choice
 * until the minimum is met -- see this file's header comment on why
 * that's needed for e.g. `(a{0}){5}`-shaped patterns); past the
 * minimum, greedily try one more repetition before trying to stop, but
 * never two consecutive zero-width ones (that's `blocked`'s job -- the
 * standard infinite-loop guard every backtracking regex engine needs
 * for e.g. `(a*)*`, which doubles here as exactly the mechanism that
 * reproduces libstdc++'s "one trailing empty iteration" capture quirk,
 * since the capture-overwrite in K_CLOSEGROUP already committed before
 * `blocked` ever gets consulted). */
static int match_rep(const RNode *rep, int64_t pos, const Cont *k, MCtx *ctx, int reps_done, int blocked) {
    if (reps_done < rep->rep_min) {
        Cont c;
        c.kind = K_STARITER;
        c.u.stariter.rep = rep;
        c.u.stariter.reps_done = reps_done;
        c.u.stariter.start_pos = pos;
        c.next = k;
        return m(rep->child, pos, &c, ctx);
    }
    if (!blocked && (rep->rep_max == -1 || reps_done < rep->rep_max)) {
        Cont c;
        c.kind = K_STARITER;
        c.u.stariter.rep = rep;
        c.u.stariter.reps_done = reps_done;
        c.u.stariter.start_pos = pos;
        c.next = k;
        if (m(rep->child, pos, &c, ctx)) return 1;
    }
    return run_cont(k, pos, ctx);
}

int asnix_regex_match_anchored(const AsnixCustomRegex *re, const char *subject, int64_t slen,
                                int64_t start, int64_t end, int notbol, AsnixRegexGroup *groups_out) {
    MCtx ctx;
    ctx.subject = subject;
    ctx.slen = slen;
    ctx.notbol = notbol;
    ctx.groups = groups_out;
    ctx.nsub = re->nsub;
    for (int64_t i = 0; i < re->nsub; i++) { groups_out[i].so = -1; groups_out[i].eo = -1; }
    ctx.anchored = 1;
    ctx.required_end = end;
    ctx.budget = NULL; /* anchored mode's RN_ALT never consults it (see its own check) */
    return m(re->root, start, NULL, &ctx);
}

/* Exploration budget for RN_ALT's search-mode "try every branch" (see
 * MCtx's own comment): generous for anything this engine is actually
 * meant to handle -- nix-conformance's regex-NNN corpus patterns are
 * all tiny -- while still bounding the genuinely-exponential case
 * (repeated ambiguous alternation over a long run) to a few hundred
 * thousand node visits total instead of letting it run unbounded. */
#define ASNIX_REGEX_SEARCH_BUDGET 300000

int asnix_regex_search(const AsnixCustomRegex *re, const char *subject, int64_t slen,
                        int64_t from_pos, int notbol, int64_t *out_start, int64_t *out_end,
                        AsnixRegexGroup *groups_out) {
    for (int64_t start = from_pos; start <= slen; start++) {
        MCtx ctx;
        ctx.subject = subject;
        ctx.slen = slen;
        ctx.notbol = notbol;
        ctx.groups = NULL; /* length-finding pass only -- captures recovered below, anchored */
        ctx.nsub = 0;
        ctx.anchored = 0;
        ctx.found = 0;
        ctx.best_end = -1;
        int64_t budget = ASNIX_REGEX_SEARCH_BUDGET;
        ctx.budget = &budget;
        m(re->root, start, NULL, &ctx);
        if (ctx.found) {
            *out_start = start;
            *out_end = ctx.best_end;
            if (groups_out && re->nsub > 0) asnix_regex_match_anchored(re, subject, slen, start, ctx.best_end, notbol, groups_out);
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------
 * `asnix_regex_needs_custom_engine` -- see the header's doc comment.
 * Bracket-expression-aware in the same style as
 * builtins_regex.c's pattern_rejected_by_real_nix (walks the pattern
 * tracking in_bracket/bracket_first state so a literal '[' that's just
 * an ordinary bracket member, e.g. "[[a]", doesn't misfire).
 * --------------------------------------------------------------------- */
int asnix_regex_needs_custom_engine(const char *p, int64_t len) {
    static const char *std12[] = { "alpha", "digit", "alnum", "upper", "lower", "space",
                                    "blank", "cntrl", "punct", "print", "graph", "xdigit" };
    int in_bracket = 0, bracket_first = 0;
    int64_t i = 0;
    while (i < len) {
        char c = p[i];
        if (!in_bracket) {
            if (c == '\\') { i += (i + 1 < len) ? 2 : 1; continue; }
            if (c == '[') {
                in_bracket = 1;
                bracket_first = 1;
                i++;
                if (i < len && p[i] == '^') i++;
                continue;
            }
            i++;
            continue;
        }
        if (bracket_first && c == ']') { bracket_first = 0; i++; continue; }
        bracket_first = 0;
        if (c == '[' && i + 1 < len && p[i + 1] == '.') {
            /* Any named collating symbol (`[.name.]`), multi-char or
             * single-char, needs the custom engine: a multi-char name
             * (e.g. `[.hyphen.]`) is a GNU/glibc leniency libstdc++
             * rejects outright; a single-char name is only valid in
             * libstdc++ when the character is alphabetic AND the symbol
             * isn't being used as a range's high endpoint (`[a-[.c.]]`,
             * `[[.a.]-[.c.]]`) -- both narrower than glibc's own
             * acceptance, so glibc's regcomp can't be trusted for
             * either case (see `collate_lookup`/`parse_range_endpoint`'s
             * own comments for the exact rules this engine enforces). */
            return 1;
        }
        if (c == '[' && i + 1 < len && p[i + 1] == '=') {
            int64_t j = i + 2;
            while (j + 1 < len && !(p[j] == '=' && p[j + 1] == ']')) j++;
            if (j + 1 >= len) { i = len; continue; }
            return 1; /* any equivalence class -- glibc's own matching is wrong for it */
        }
        if (c == '[' && i + 1 < len && p[i + 1] == ':') {
            int64_t j = i + 2;
            while (j + 1 < len && !(p[j] == ':' && p[j + 1] == ']')) j++;
            if (j + 1 >= len) { i = len; continue; }
            int64_t namelen = j - (i + 2);
            int is_exact_std = 0;
            for (size_t k = 0; k < sizeof std12 / sizeof std12[0]; k++) {
                if ((int64_t)strlen(std12[k]) == namelen && memcmp(p + i + 2, std12[k], (size_t)namelen) == 0) {
                    is_exact_std = 1;
                    break;
                }
            }
            if (!is_exact_std && namelen > 0 && namelen < 16) {
                char lname[16];
                for (int64_t t = 0; t < namelen; t++) lname[t] = (char)tolower((unsigned char)p[i + 2 + t]);
                lname[namelen] = '\0';
                int known = (strcmp(lname, "w") == 0);
                for (size_t k = 0; k < sizeof std12 / sizeof std12[0] && !known; k++)
                    if (strcmp(lname, std12[k]) == 0) known = 1;
                if (known) return 1; /* case variant of a standard name, or the GNU 'w' extension */
            }
            i = j + 2;
            continue;
        }
        if (c == ']') { in_bracket = 0; i++; continue; }
        i++;
    }
    return 0;
}
