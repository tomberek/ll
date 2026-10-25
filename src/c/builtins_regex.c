/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
/*
 * builtins.match / builtins.split -- see builtins_regex.h. Both verified
 * against real `nix-instantiate --eval --strict` (2.35.2) before being
 * trusted; see each function's own comment for the specific expressions
 * checked, and /tmp/nix-conformance/lang/corpora/regex.nix for the full
 * breadth of POSIX ERE shapes this is meant to cover.
 *
 * M68 added `regex_engine.c`, a small hand-written backtracking ERE
 * matcher, alongside the glibc-based matching below -- see its header
 * comment for the full rationale (glibc's regexec and real Nix's
 * libstdc++ `std::regex` disagree on which capture a repeated/ambiguous
 * subexpression gets, and glibc can't compile a few bracket-expression
 * extensions libstdc++ accepts). Routing, in both match2_impl and
 * split2_impl below: a pattern with zero capture groups never touches
 * the custom engine at all (nothing for it to get right that glibc
 * doesn't already); a pattern using one of the bracket-expression
 * extensions bypasses glibc entirely (`asnix_regex_needs_custom_engine`);
 * everything else still uses glibc's regexec to find the match's
 * overall span (that part was never wrong) and only asks the custom
 * engine to recompute that span's capture groups
 * (`groups_to_list_recomputed`), falling back to glibc's own group
 * results unchanged if the custom engine can't parse the pattern at all
 * (should not happen for anything regcomp itself accepted, but costs
 * nothing to guard defensively -- see that function's own comment).
 *
 * M184: `asnix_prim_match2_impl` additionally makes the custom engine
 * AUTHORITATIVE for the match/no-match decision ITSELF (not just
 * capture recompute), skipping regexec()'s own EXECUTION (not just
 * regcomp -- M143 already made that free after the first call per
 * distinct pattern) once a given pattern's custom-engine existence
 * verdict has been empirically proven, over several real calls, to
 * agree with glibc's -- see RegexCacheEntry's `span_trusted` field and
 * docs/architecture.md's M184 section for the full correctness
 * argument (short version: `match`'s own question, "does the WHOLE
 * fixed-length string match", is answered correctly by ANY complete
 * backtracking search regardless of which decomposition it prefers
 * when more than one exists -- the ambiguity M68 found only affects
 * WHICH capture wins, never whether a full-length match exists at
 * all -- so this is safe against the alternation/repetition ambiguity
 * class; the per-pattern empirical verification window exists as
 * defense-in-depth against plain parser bugs in regex_engine.c's own
 * narrower grammar, not against ambiguity). `split2_impl` is
 * deliberately NOT changed by M184: its unanchored search can
 * genuinely disagree with glibc on the match SPAN itself even without
 * capture groups (top-level alternation's longest-vs-ordered
 * preference can differ between libstdc++ and glibc in search mode --
 * see M68's quirk 3 and the M184 writeup's own worked example), a
 * fundamentally different and harder-to-bound risk than match's
 * tie-break-independent existence question.
 */
#include "builtins_regex.h"

#include <regex.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/value.h"
#include "builtins_shared.h"
#include "regex_engine.h"
#include "value_util.h"

/* Nix strings aren't NUL-terminated (heap.h/value_util.h's
 * asnix_ptr_bytes -- explicit length), but regcomp/regexec need C
 * strings. Plain malloc/free scratch copy (freed by every caller before
 * returning) -- not arena-allocated, since nothing here needs to outlive
 * this one builtin call, same convention builtins_compare.c's
 * replaceStrings/toJSON already use for their own scratch buffers. */
static char *nul_copy(const char *bytes, int64_t len) {
    char *buf = malloc((size_t)len + 1);
    if (len > 0) memcpy(buf, bytes, (size_t)len);
    buf[len] = '\0';
    return buf;
}

/* Real Nix's regex engine is libstdc++'s `std::regex` (built with
 * `std::regex::extended`), not glibc's `<regex.h>` -- the two disagree on
 * the validity of a specific, small family of patterns that are outside
 * strict POSIX ERE but that glibc's `regcomp` happens to accept anyway as
 * a GNU extension/leniency. Found via nix-conformance's
 * `lang/corpora/regex-invalid.nix` (EREs "which libstdc++ rejects" --
 * every entry there is cross-checked against real `nix-instantiate`,
 * which does reject all of them): without this pre-check, `regcomp`
 * alone would accept several of them, so `builtins.match "\\n" "x"`
 * would wrongly succeed/return `null` here instead of being the fatal
 * error real Nix gives. Checked directly against real nix-instantiate
 * (see /tmp/check_escapes.sh-style probing) for the exact boundary of
 * each rule below before trusting it:
 *
 * - Escaping anything other than the 12 real POSIX ERE metacharacters
 *   (`. ( ) * + ? [ { ^ $ | \`) is rejected -- this is the single
 *   biggest category: `\n` `\w` `\d` `\b` `\/` (GNU-extension-only
 *   shorthand escapes glibc accepts, verified real Nix rejects all of
 *   them) and, perhaps surprisingly, `\]` and `\}` too (confirmed
 *   directly: `\(` `\)` `\[` `\{` are all valid escapes but `\]` and `\}`
 *   are NOT -- asymmetric, but that's genuinely what real Nix does,
 *   since `]` and `}` aren't "special" on their own in POSIX ERE grammar,
 *   only in context after a matching `[`/`{`). A backreference like
 *   `\1` falls under this same rule (a digit isn't in the 12-char list)
 *   -- POSIX ERE has no backreference construct at all (unlike BRE's
 *   `\1`..`\9`), so this isn't a separate "backreferences unsupported"
 *   scope cut, just the same escape-whitelist rule catching it for free.
 * - `{` immediately followed by `,` (e.g. `a{,2}`, missing the required
 *   lower bound) -- a GNU-extension-lenient interval form glibc accepts
 *   (treating the missing bound as 0) that real Nix rejects; POSIX ERE
 *   intervals are `{n}`/`{n,}`/`{n,m}`, `n` is not optional.
 * - An unmatched `)` with no corresponding `(` (e.g. `a)`) -- glibc
 *   treats a lone `)` as an ordinary/literal character (same leniency it
 *   gives unmatched `]`/`}`); real Nix rejects it.
 *
 * Bracket-expression-aware (POSIX `[...]`/`[^...]`, including the
 * leading-`]`-is-literal rule and `[. .]`/`[: :]`/`[= =]` sub-expressions)
 * so none of the above rules misfire on a `(`/`)`/`{`/`\` that's actually
 * just an ordinary bracket member (e.g. `[\n]` -- a literal backslash and
 * `n`, two bracket members, not an escape sequence at all, since
 * backslash has no special meaning inside `[...]` in POSIX ERE -- stays
 * valid, matching real Nix).
 *
 * `[[.-.]]`/`[a-[.c.]]`-shaped obscure POSIX collating-symbol forms
 * (non-alphabetic single-char collating names, and any collating
 * symbol used as a range's HIGH endpoint) WERE chased, as of M80 --
 * see regex_engine.c's `collate_lookup`/`parse_range_endpoint`/
 * `asnix_regex_needs_custom_engine` and docs/architecture.md's M80
 * scope notes for the exact rules and how `asnix_regex_needs_custom_
 * engine` routes every `[.name.]` occurrence to that engine rather
 * than trusting glibc's more lenient `regcomp` for it.
 *
 * M82 (see docs/architecture.md): `(a){24998}` -- real Nix rejects this
 * specific group-repetition count as "memory limit exceeded" at
 * libstdc++'s own internal NFA-state-count limit, well below glibc's
 * own much higher limit (which still accepts it, so unlike
 * `a{99999}`-shaped BARE-repetition cases -- no group, just a literal
 * repeated directly -- where glibc's own regcomp limit happens to trip
 * at a nearby count for unrelated reasons and asnix already gets this
 * right "for free" via regcomp's own rejection, a *group* repetition
 * needs its own explicit check here). Confirmed empirically against the
 * `nix-instantiate` installed in this environment (binary search on
 * `builtins.match "(a){N}" "aaa"`): accepted up to exactly N=24997,
 * rejected at N=24998 -- an inherent engine-complexity-limit whose exact
 * boundary depends on the repeated body's own shape (a costlier body
 * would trip at a lower N) in a way that can't be derived in general
 * from a handful of probe patterns without effectively reverse-
 * engineering libstdc++'s NFA compiler's internal state-cost accounting
 * -- NOT attempted here. Instead: `group_repeat_bound_too_large` below
 * rejects any `(...)` capture group followed by a `{m}`/`{m,}`/`{m,n}`
 * interval whose bound exceeds `ASNIX_GROUP_REPEAT_SAFE_MAX` (1000) --
 * comfortably above every actual group-repetition count anywhere in
 * numtide/nix-conformance's corpora (`corpora/regex.nix`'s own group-
 * repeat cases top out at `{1,3}`) and comfortably below the real
 * ~24998 limit, so this can never misclassify anything currently
 * exercised in either direction; it is a conservative approximation of
 * real Nix's actual limit, not a faithful reproduction of it. */
#define ASNIX_GROUP_REPEAT_SAFE_MAX 1000

/* `p[i] == '{'` on entry (caller already checked). Parses a POSIX ERE
 * interval `{m}`/`{m,}`/`{m,n}` starting there and returns 1 if either
 * bound present exceeds ASNIX_GROUP_REPEAT_SAFE_MAX -- see the comment
 * above. Returns 0 (not rejected on this basis) for anything else,
 * including a malformed interval (that's `pattern_rejected_by_real_nix`'s
 * OTHER rules', or plain regcomp's, concern -- this function only ever
 * adds rejections, never overrides one already found valid). */
static int group_repeat_bound_too_large(const char *p, int64_t len, int64_t i) {
    int64_t j = i + 1;
    int64_t bound1 = -1, bound2 = -1;
    int64_t start = j;
    while (j < len && p[j] >= '0' && p[j] <= '9') j++;
    if (j > start) bound1 = atoll(p + start); /* no overflow concern: capped against a plain 4-digit threshold below */
    if (j < len && p[j] == ',') {
        j++;
        start = j;
        while (j < len && p[j] >= '0' && p[j] <= '9') j++;
        if (j > start) bound2 = atoll(p + start);
    }
    if (j >= len || p[j] != '}') return 0; /* malformed -- not this function's rule to enforce */
    return (bound1 > ASNIX_GROUP_REPEAT_SAFE_MAX) || (bound2 > ASNIX_GROUP_REPEAT_SAFE_MAX);
}
static int pattern_rejected_by_real_nix(const char *p, int64_t len) {
    static const char *valid_escapes = ".()*+?[{^$|\\";
    int in_bracket = 0, bracket_first = 0, paren_depth = 0;
    int64_t i = 0;
    while (i < len) {
        char c = p[i];
        if (!in_bracket) {
            if (c == '\\') {
                if (i + 1 >= len) return 0; /* trailing '\' -- glibc itself already rejects this (rc=5) */
                if (!strchr(valid_escapes, p[i + 1])) return 1;
                i += 2;
                continue;
            }
            if (c == '(') { paren_depth++; i++; continue; }
            if (c == ')') {
                if (paren_depth == 0) return 1;
                paren_depth--;
                i++;
                /* M82: a just-closed group immediately followed by a
                 * large `{m}`/`{m,}`/`{m,n}` interval -- see
                 * group_repeat_bound_too_large's own comment above. */
                if (i < len && p[i] == '{' && group_repeat_bound_too_large(p, len, i)) return 1;
                continue;
            }
            if (c == '{' && i + 1 < len && p[i + 1] == ',') return 1;
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
        /* inside a bracket expression -- '\', '(', ')', '{' are all
         * ordinary members here, not special (POSIX ERE rule). */
        if (bracket_first && c == ']') {
            bracket_first = 0;
            i++;
            continue;
        }
        bracket_first = 0;
        if (c == '[' && i + 1 < len && (p[i + 1] == '.' || p[i + 1] == ':' || p[i + 1] == '=')) {
            char kind = p[i + 1];
            int64_t j = i + 2;
            while (j + 1 < len && !(p[j] == kind && p[j + 1] == ']')) j++;
            if (j + 1 >= len) return 0; /* unterminated -- glibc itself already rejects this */
            i = j + 2;
            continue;
        }
        if (c == ']') {
            in_bracket = 0;
            i++;
            continue;
        }
        i++;
    }
    return 0; /* unterminated '[' / unmatched '(' are already glibc's own rejections */
}

/* M143: a pattern-string-keyed cache for both this file's compiled
 * regex forms -- glibc's `regex_t` (via `regcomp`) and regex_engine.c's
 * own `AsnixCustomRegex` -- found via a cross-evaluator profiling pass
 * (see docs/architecture.md's M143 section for the full writeup):
 * `regcomp` alone builds a full DFA plus a locale character-class
 * translation table from scratch on every call, and real nixpkgs calls
 * `builtins.match`/`builtins.split` with the SAME small set of literal
 * patterns (lib.versions-style version-parsing helpers) across
 * thousands of packages in one closure, so recompiling the identical
 * pattern string thousands of times was pure waste. Entries are never
 * evicted or freed for the life of the process -- the same unbounded
 * per-process tradeoff iets's own thread-local `REGEX_CACHE` already
 * makes; the number of DISTINCT patterns a real eval calls match/split
 * with is small relative to how many TIMES each one is called, so the
 * memory cost is negligible next to the win. Same open-addressed,
 * linear-probe hash-index technique eval_file.c's import cache already
 * uses (`import_cache_find`/`import_cache_insert`, see its own comment):
 * a parallel index array mapping each pattern's hash to its slot in
 * `g_regex_cache`, grown/rehashed at a 75% load factor before each
 * insert. */
typedef struct {
    char *pattern;   /* owned copy, exactly `len` bytes, no NUL terminator */
    int64_t len;

    int rejected;      /* pattern_rejected_by_real_nix(pattern, len), computed once */
    int needs_custom;   /* asnix_regex_needs_custom_engine(pattern, len), computed once */

    int have_glibc;       /* glibc_re is a successfully regcomp'd pattern */
    int glibc_bad;         /* regcomp itself failed -- glibc_err is the message to replay */
    char *glibc_err;        /* owned (regerror's own buffer is caller-provided/transient) */
    regex_t glibc_re;

    int custom_attempted;     /* asnix_regex_compile already tried once (success or not) */
    AsnixCustomRegex *custom_re; /* compiled form, or NULL if the attempt failed */
    int64_t custom_nsub;

    /* M184: per-pattern empirical trust state for making the custom
     * engine AUTHORITATIVE for `match`'s whole-string span decision
     * itself (not just capture recompute for a span glibc already
     * found) -- see this file's top comment and the M184 writeup in
     * docs/architecture.md for the full rationale. Tri-state:
     *   0  -- still verifying: the next several calls with this exact
     *         pattern cross-check the custom engine's existence verdict
     *         against glibc's regexec() on the SAME call, and the
     *         glibc-derived result (already correct, unchanged) is what
     *         gets returned while still in this state.
     *   1  -- trusted: `span_verify_count` consecutive calls all agreed,
     *         so `asnix_prim_match2_impl` skips regexec() entirely for
     *         later calls with this pattern and trusts the custom
     *         engine's own existence verdict directly.
     *  -1  -- proven unsafe (a disagreement was actually observed, or
     *         the custom engine can't compile this pattern / reports a
     *         different group count than regcomp) -- permanently falls
     *         back to the regexec()-driven path for this pattern, same
     *         as before this milestone, forever. */
    int span_trusted;
    int span_verify_count;
} RegexCacheEntry;

static RegexCacheEntry **g_regex_cache = NULL;
static size_t g_regex_cache_count = 0, g_regex_cache_cap = 0;
static size_t *g_regex_cache_index = NULL;
static size_t g_regex_cache_index_cap = 0;
#define REGEX_CACHE_EMPTY_SLOT ((size_t)-1)

/* Same word-at-a-time mixing as eval_file.c's import_cache_hash/
 * symtab.c's hash_bytes (M130). */
static uint64_t regex_cache_hash(const char *s, int64_t len) {
    uint64_t h = 0;
    for (int64_t i = 0; i < len; i++) {
        h ^= (uint64_t)(unsigned char)s[i];
        h *= 0x517cc1b727220a95ULL;
    }
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebULL;
    h ^= h >> 31;
    return h;
}

static void regex_cache_index_rehash(size_t new_cap) {
    size_t *new_table = malloc(new_cap * sizeof(size_t));
    for (size_t i = 0; i < new_cap; i++) new_table[i] = REGEX_CACHE_EMPTY_SLOT;
    for (size_t i = 0; i < g_regex_cache_count; i++) {
        RegexCacheEntry *e = g_regex_cache[i];
        size_t slot = regex_cache_hash(e->pattern, e->len) & (new_cap - 1);
        while (new_table[slot] != REGEX_CACHE_EMPTY_SLOT) slot = (slot + 1) & (new_cap - 1);
        new_table[slot] = i;
    }
    free(g_regex_cache_index);
    g_regex_cache_index = new_table;
    g_regex_cache_index_cap = new_cap;
}

static RegexCacheEntry *regex_cache_find(const char *pattern, int64_t len) {
    if (g_regex_cache_index_cap == 0) return NULL;
    size_t slot = regex_cache_hash(pattern, len) & (g_regex_cache_index_cap - 1);
    for (;;) {
        size_t idx = g_regex_cache_index[slot];
        if (idx == REGEX_CACHE_EMPTY_SLOT) return NULL;
        RegexCacheEntry *e = g_regex_cache[idx];
        if (e->len == len && memcmp(e->pattern, pattern, (size_t)len) == 0) return e;
        slot = (slot + 1) & (g_regex_cache_index_cap - 1);
    }
}

static RegexCacheEntry *regex_cache_insert(const char *pattern, int64_t len) {
    RegexCacheEntry *e = malloc(sizeof *e);
    e->pattern = malloc(len > 0 ? (size_t)len : 1);
    if (len > 0) memcpy(e->pattern, pattern, (size_t)len);
    e->len = len;
    e->rejected = pattern_rejected_by_real_nix(pattern, len);
    e->needs_custom = asnix_regex_needs_custom_engine(pattern, len);
    e->have_glibc = 0;
    e->glibc_bad = 0;
    e->glibc_err = NULL;
    e->custom_attempted = 0;
    e->custom_re = NULL;
    e->custom_nsub = 0;
    e->span_trusted = 0;
    e->span_verify_count = 0;

    if (g_regex_cache_count == g_regex_cache_cap) {
        g_regex_cache_cap = g_regex_cache_cap ? g_regex_cache_cap * 2 : 32;
        g_regex_cache = realloc(g_regex_cache, g_regex_cache_cap * sizeof(RegexCacheEntry *));
    }
    if (g_regex_cache_index_cap == 0) regex_cache_index_rehash(64);
    else if ((g_regex_cache_count + 1) * 4 > g_regex_cache_index_cap * 3) regex_cache_index_rehash(g_regex_cache_index_cap * 2);
    size_t slot = regex_cache_hash(e->pattern, e->len) & (g_regex_cache_index_cap - 1);
    while (g_regex_cache_index[slot] != REGEX_CACHE_EMPTY_SLOT) slot = (slot + 1) & (g_regex_cache_index_cap - 1);
    g_regex_cache_index[slot] = g_regex_cache_count;
    g_regex_cache[g_regex_cache_count] = e;
    g_regex_cache_count++;
    return e;
}

static RegexCacheEntry *regex_cache_get(const char *pattern, int64_t len) {
    RegexCacheEntry *e = regex_cache_find(pattern, len);
    if (e) return e;
    return regex_cache_insert(pattern, len);
}

/* Fatal on a bad pattern -- matches real Nix's own fatal-on-bad-regcomp
 * behavior (verified: `builtins.match "(" "a"` -> "error: invalid
 * regular expression '('" on real nix-instantiate; exact wording isn't
 * reproduced, just the fatal-not-null outcome, same class of
 * documented-not-exact-wording scope cut every other fatal error in this
 * project already makes).
 *
 * M143: cache-aware -- `e->rejected` was already computed once at
 * cache-insert time; a pattern `regcomp` itself rejects caches that
 * fact too (`glibc_bad`/`glibc_err`), so a repeated bad pattern replays
 * the exact same fatal message without re-running `regcomp` just to
 * refail it. `pattern` must still be the caller's own NUL-terminated
 * scratch copy (`e->pattern` is NOT NUL-terminated -- see above), since
 * `regcomp` needs a C string. */
static regex_t *regex_cache_ensure_glibc(RegexCacheEntry *e, const char *pattern, const char *who) {
    if (e->rejected) {
        asnix_bshared_fatal(who, "invalid regular expression (rejected by real Nix's stricter ERE grammar)");
    }
    if (e->glibc_bad) {
        asnix_bshared_fatal(who, e->glibc_err);
    }
    if (e->have_glibc) return &e->glibc_re;
    int rc = regcomp(&e->glibc_re, pattern, REG_EXTENDED);
    if (rc != 0) {
        char errbuf[256];
        regerror(rc, &e->glibc_re, errbuf, sizeof errbuf);
        e->glibc_bad = 1;
        size_t elen = strlen(errbuf);
        e->glibc_err = malloc(elen + 1);
        memcpy(e->glibc_err, errbuf, elen + 1);
        asnix_bshared_fatal(who, e->glibc_err);
    }
    e->have_glibc = 1;
    return &e->glibc_re;
}

/* M143: cache-aware wrapper around regex_engine.c's `asnix_regex_compile`
 * -- compiled at most once per distinct pattern string, reused by every
 * later call (both the needs-custom-engine full-bypass route and
 * `groups_to_list_recomputed`'s per-match capture recompute, which
 * previously called `asnix_regex_compile` fresh on EVERY match inside a
 * single `split` call, not just once per distinct pattern). */
static AsnixCustomRegex *regex_cache_ensure_custom(RegexCacheEntry *e, const char *pattern, int64_t plen) {
    if (e->custom_attempted) return e->custom_re;
    e->custom_attempted = 1;
    e->custom_re = asnix_regex_compile(pattern, plen, &e->custom_nsub);
    return e->custom_re;
}

/* Builds the flat list of `nsub` capture-group values from a completed
 * regexec()'s pmatch[] array (pmatch[0] is the whole match, pmatch[1..nsub]
 * are the groups -- same indexing regexec itself uses). `base` is the
 * pointer regexec was actually called against (subject, or subject+pos
 * for split's windowed search below) -- pmatch offsets are relative to
 * whatever pointer regexec saw, not necessarily the start of the whole
 * subject. A group that didn't participate in the match (rm_so == -1 --
 * e.g. an alternation branch not taken) becomes `null` in the list, not
 * an empty string: verified directly, `builtins.match "(a)?(b)" "b"` ==
 * `[ null "b" ]`. */
static Value groups_to_list(const regmatch_t *pmatch, size_t nsub, const char *base) {
    Value *vals = nsub > 0 ? malloc(nsub * sizeof(Value)) : NULL;
    for (size_t i = 0; i < nsub; i++) {
        regmatch_t m = pmatch[i + 1];
        if (m.rm_so < 0) {
            vals[i] = ASNIX_NULL;
        } else {
            vals[i] = asnix_make_string_value(base + m.rm_so, (size_t)(m.rm_eo - m.rm_so));
        }
    }
    Value result = asnix_bshared_make_list(vals, (int64_t)nsub);
    free(vals);
    return result;
}

/* Same shape as groups_to_list, but from the custom engine's own
 * AsnixRegexGroup[] (always absolute offsets into `subject`, never
 * relative to a search window -- regex_engine.c never slices/copies the
 * subject, see its header comment) rather than regexec's pmatch[]. */
static Value custom_groups_to_list(const AsnixRegexGroup *g, int64_t nsub, const char *subject) {
    Value *vals = nsub > 0 ? malloc((size_t)nsub * sizeof(Value)) : NULL;
    for (int64_t i = 0; i < nsub; i++) {
        if (g[i].so < 0) {
            vals[i] = ASNIX_NULL;
        } else {
            vals[i] = asnix_make_string_value(subject + g[i].so, (size_t)(g[i].eo - g[i].so));
        }
    }
    Value result = asnix_bshared_make_list(vals, nsub);
    free(vals);
    return result;
}

/* regex_engine.c's matcher is plain recursive-descent-over-the-AST CPS
 * backtracking (see its header comment) -- a `*`/`+`/`{m,}` repeated
 * over N real (non-empty) iterations costs O(N) *C call stack* depth,
 * not just O(N) time, since each iteration recurses into the next
 * rather than looping. Confirmed directly (a dedicated probe harness,
 * not part of this project's own test suite) that this starts
 * segfaulting somewhere in the low thousands of matched characters even
 * for simple patterns, and for `((a|b)(c|d))+`-style nested-group ones,
 * somewhat sooner -- consistent with nix-conformance's own
 * eval-okay-regex-264 (`(a|b)*` against a >10000-character string)
 * crashing when this milestone's first pass wired the custom engine's
 * recompute in unconditionally. A real fix (converting repetition to an
 * explicit heap-allocated backtracking stack instead of C recursion) is
 * out of scope for this pass -- see this file's M68 scope notes; this
 * cap is the deliberate, documented mitigation instead: well below the
 * measured crash threshold (which already has a comfortable safety
 * margin baked in, and this process has *less* free stack than the
 * standalone probe did, being already deep in asnix's own evaluator
 * call stack by the time a builtin runs), while still generously above
 * every pattern this milestone actually targets (the full 27+6-test
 * Class A/B corpus is all single-digit-to-low-double-digit-character
 * inputs -- nothing resembling nix-conformance's separate "long
 * inputs" corpus section, which exists to stress leftmost-longest
 * whole-match-finding, not subexpression capture, and was already
 * correctly handled by glibc's regexec before this milestone). Above
 * this cap, `groups_to_list_recomputed` falls back to glibc's own
 * (unimproved, but also unregressed) `pmatch`-based capture groups. */
#define ASNIX_REGEX_CUSTOM_MAX_SPAN 2000

/* M184: number of consecutive agreeing calls (per distinct pattern)
 * required before `asnix_prim_match2_impl` trusts the custom engine's
 * own existence verdict and skips regexec() entirely for later calls --
 * see RegexCacheEntry's `span_trusted` field for the full scheme. A
 * small number is enough: real nixpkgs evals call match/split with a
 * small set of DISTINCT patterns repeated across thousands of packages
 * (M143's own finding), so this one-time per-pattern verification cost
 * is negligible next to the win on every later call; a handful of
 * different subjects (not just one) also guards specifically against
 * the "first subject got lucky" risk a single-call check would have. */
#define ASNIX_REGEX_SPAN_VERIFY_CALLS 8

/* Given a match glibc's regexec already found spans `[match_so,
 * match_eo)` of the full `subject` (absolute offsets -- `match_so`/
 * `match_eo` are NOT relative to `base`, see groups_to_list's own
 * comment on what `base` means for split's windowed calls), recomputes
 * that match's capture groups with the custom engine instead of trusting
 * regexec's own `pmatch[1..nsub]` -- see this file's top-of-file comment
 * and regex_engine.c's header comment for why. `notbol` mirrors
 * REG_NOTBOL (split's windowed re-search from pos > 0 passes 1).
 *
 * Falls back to groups_to_list's plain (glibc) result, unchanged, if the
 * match span is too long to safely recurse over (see
 * ASNIX_REGEX_CUSTOM_MAX_SPAN above) or if the custom engine can't parse
 * `pattern` at all or reports a different group count than regcomp did
 * -- the latter two should never happen for anything regcomp itself
 * already accepted (this project's own hand-rolled parser is
 * deliberately narrower in scope, see regex_engine.c, but is meant to
 * be a strict superset of what matters here), but falling back
 * defensively means a gap in that parser this project's test suite
 * doesn't happen to exercise would just leave the affected pattern
 * exactly as correct/incorrect as it already was before this milestone,
 * never introduce a NEW regression.
 *
 * M143: `e` is the pattern's cache entry (same one the caller already
 * resolved via `regex_cache_get`) -- the custom-engine form is compiled
 * at most once per distinct pattern (`regex_cache_ensure_custom`), not
 * once per match, which matters a lot here specifically: `split` calls
 * this function once per match found in the subject, so an N-match
 * split previously re-ran `asnix_regex_compile` N times for the exact
 * same pattern string. */
static Value groups_to_list_recomputed(const regmatch_t *pmatch, size_t nsub, const char *base,
                                        RegexCacheEntry *e, const char *pattern, int64_t plen,
                                        const char *subject, int64_t slen, int64_t match_so,
                                        int64_t match_eo, int notbol) {
    if (nsub == 0) return groups_to_list(pmatch, nsub, base);
    if (match_eo - match_so > ASNIX_REGEX_CUSTOM_MAX_SPAN) return groups_to_list(pmatch, nsub, base);
    AsnixCustomRegex *cre = regex_cache_ensure_custom(e, pattern, plen);
    if (cre && e->custom_nsub == (int64_t)nsub) {
        AsnixRegexGroup *g = malloc(nsub * sizeof *g);
        int ok = asnix_regex_match_anchored(cre, subject, slen, match_so, match_eo, notbol, g);
        if (ok) {
            Value r = custom_groups_to_list(g, (int64_t)nsub, subject);
            free(g);
            return r;
        }
        free(g);
    }
    return groups_to_list(pmatch, nsub, base);
}

/* ---------------------------------------------------------------------
 * builtins.match regex str -- real Nix implicitly anchors `regex` to the
 * ENTIRE string (as if wrapped in `^...$`), returning `null` if it
 * doesn't match end-to-end, or the flat list of the regex's own capture
 * groups (NOT including the whole match) if it does.
 *
 * Rather than literally wrapping the pattern text in `^(?:...)$` (POSIX
 * ERE has no non-capturing group -- `\(...\)` IS a capture group, which
 * would shift every real group's index by one), this relies on a
 * property of POSIX regexec() itself: it always returns the match that
 * starts at the *earliest* possible position in the string, and -- for
 * any fixed start position -- the *longest* match starting there
 * (leftmost-longest, REG_EXTENDED's whole point). So if a match exists
 * starting at offset 0, regexec() is guaranteed to find one starting at
 * offset 0 (nothing can be "earlier"), and that match is guaranteed to
 * be the LONGEST one starting at offset 0. Therefore: call regexec()
 * plain (no pattern rewriting, no flags), and check whether the single
 * match it reports happens to start at 0 and end at the full length --
 * if the longest match starting at 0 doesn't reach the end, no
 * whole-string match is possible at all, so `null` is correct either
 * way. This is exactly `std::regex_match`'s own semantics (which is what
 * real Nix's primops.cc actually calls), just reimplemented against
 * POSIX regexec() instead of libstdc++'s regex engine.
 *
 * Verified against real nix-instantiate: `match "foo" "foo"` == `[]`
 * (no groups); `match "(f)(o+)" "foo"` == `[ "f" "oo" ]`; `match "(a)?(b)"
 * "b"` == `[ null "b" ]` (unparticipated group is null, not ""); `match
 * "a|ab" "ab"` == `[]` (leftmost-longest picks the full "ab" branch, a
 * whole-string match -- Perl-style leftmost-first-branch would have tried
 * "a" first, matched only a prefix, and returned null instead); `match "a"
 * ""` == null; `match "" ""` == `[]`; `match "." "a\nc"`-shaped: `.`
 * matches a literal newline too (no REG_NEWLINE passed, matching real
 * Nix's own regex construction, which doesn't set multiline/dotall-excl
 * flags either -- verified `match "a.c" "a\nc"` == `[]`, i.e. matches).
 * --------------------------------------------------------------------- */
Value asnix_prim_match2_impl(const Value *args, int64_t argc) {
    (void)argc;
    int64_t plen, slen;
    const char *pbytes, *sbytes;
    asnix_bshared_string_bytes(args[0], &pbytes, &plen, "match");
    asnix_bshared_string_bytes(args[1], &sbytes, &slen, "match");

    char *pattern = nul_copy(pbytes, plen);
    char *subject = nul_copy(sbytes, slen);

    Value result;

    /* M143: resolve (or create) this pattern's cache entry once, up
     * front -- `needs_custom`/`rejected` were computed at insert time,
     * so neither branch below re-scans the pattern text on a repeat
     * call with the same bytes. */
    RegexCacheEntry *re = regex_cache_get(pattern, plen);

    /* Bracket-expression extension (named collating symbol, equivalence
     * class, or non-standard class name) glibc can't be trusted for at
     * all -- bypass it entirely, same leftmost-longest-anchoring trick
     * as the glibc path below, just driven by the custom engine's own
     * from-scratch search instead of regexec. */
    if (re->needs_custom) {
        if (re->rejected) {
            asnix_bshared_fatal("match", "invalid regular expression (rejected by real Nix's stricter ERE grammar)");
        }
        AsnixCustomRegex *cre = regex_cache_ensure_custom(re, pattern, plen);
        if (!cre) asnix_bshared_fatal("match", "invalid regular expression");
        free(pattern);
        int64_t nsub = re->custom_nsub;

        AsnixRegexGroup *g = nsub > 0 ? malloc((size_t)nsub * sizeof *g) : NULL;
        int64_t mso, meo;
        int found = asnix_regex_search(cre, subject, slen, 0, 0, &mso, &meo, g);
        if (!found || mso != 0 || meo != slen) {
            result = ASNIX_NULL;
        } else {
            result = custom_groups_to_list(g, nsub, subject);
        }
        free(g);
        free(subject);
        return result;
    }

    regex_t *preg = regex_cache_ensure_glibc(re, pattern, "match");

    size_t nsub = preg->re_nsub;

    /* M184: once this exact pattern's custom-engine existence verdict
     * has been empirically proven to agree with glibc's regexec() (see
     * RegexCacheEntry's `span_trusted` comment above), skip regexec()
     * entirely -- it's the measured-expensive part (M182's follow-up
     * finding: `re_search_internal`/`re_acquire_state`/etc., not
     * `regcomp`, which M143 already made effectively free after the
     * first call). Falls through to the glibc-driven path below
     * (unchanged) whenever not yet trusted, proven unsafe, the custom
     * engine can't compile this pattern, or the subject is too long to
     * safely recurse over. */
    if (re->span_trusted == 1 && slen <= ASNIX_REGEX_CUSTOM_MAX_SPAN) {
        AsnixCustomRegex *cre = regex_cache_ensure_custom(re, pattern, plen);
        if (cre && re->custom_nsub == (int64_t)nsub) {
            AsnixRegexGroup *g = nsub > 0 ? malloc(nsub * sizeof *g) : NULL;
            int ok = asnix_regex_match_anchored(cre, subject, slen, 0, slen, 0, g);
            if (ok) {
                result = nsub > 0 ? custom_groups_to_list(g, (int64_t)nsub, subject) : asnix_bshared_make_list(NULL, 0);
            } else {
                result = ASNIX_NULL;
            }
            free(g);
            free(pattern);
            free(subject);
            return result;
        }
        /* Shouldn't happen (span_trusted==1 means this already
         * succeeded on a prior call for the SAME cached entry, and
         * compile success/nsub are deterministic properties of the
         * pattern text alone) -- defensively fall through to the
         * glibc path below rather than trust a half-formed state. */
    }

    regmatch_t *pmatch = malloc((nsub + 1) * sizeof(regmatch_t));
    int mrc = regexec(preg, subject, nsub + 1, pmatch, 0);
    int glibc_ok = (mrc == 0 && pmatch[0].rm_so == 0 && pmatch[0].rm_eo == (regoff_t)slen);

    /* M184: still in the verification window for this pattern --
     * cross-check the custom engine's own existence verdict against
     * glibc's (just computed above) on this SAME call/subject, without
     * changing what this call itself returns (the glibc-derived result
     * below is used regardless, exactly as before this milestone). */
    if (re->span_trusted == 0 && slen <= ASNIX_REGEX_CUSTOM_MAX_SPAN) {
        AsnixCustomRegex *cre = regex_cache_ensure_custom(re, pattern, plen);
        if (cre && re->custom_nsub == (int64_t)nsub) {
            AsnixRegexGroup *g = nsub > 0 ? malloc(nsub * sizeof *g) : NULL;
            int custom_ok = asnix_regex_match_anchored(cre, subject, slen, 0, slen, 0, g);
            if (custom_ok != glibc_ok) {
                re->span_trusted = -1; /* diverged for real -- never trust this pattern's span */
            } else {
                re->span_verify_count++;
                if (re->span_verify_count >= ASNIX_REGEX_SPAN_VERIFY_CALLS) re->span_trusted = 1;
            }
            free(g);
        } else {
            re->span_trusted = -1; /* can't compile under the custom engine, or a differing group count */
        }
    }

    if (!glibc_ok) {
        result = ASNIX_NULL;
    } else {
        result = groups_to_list_recomputed(pmatch, nsub, subject, re, pattern, plen, subject, slen, 0, slen, 0);
    }

    free(pattern);
    free(pmatch);
    free(subject);
    return result;
}
static Value build_match(void) { return asnix_bshared_make_curried_native((void *)asnix_prim_match2_impl, 2); }

/* ---------------------------------------------------------------------
 * builtins.split regex str -- finds every non-overlapping match of
 * `regex` ANYWHERE in `str` (not anchored), returning a flat list
 * alternating [literalTextBeforeMatch, listOfCaptureGroupsForThatMatch,
 * ..., finalTrailingLiteralText]. A pattern with no capture groups still
 * produces an empty list `[]` at each match position (not omitted).
 *
 * `pos` tracks where the next regexec() search window starts;
 * `last_end` tracks the end of the last match actually recorded (the
 * two diverge exactly when a zero-width match was just processed).
 * REG_NOTBOL is passed whenever pos > 0 so a literal `^` in the pattern
 * only ever matches the true start of the whole subject, not wherever
 * the search window happens to start (standard "search from offset N"
 * convention, same as e.g. GNU grep's own multi-match scanning) --
 * REG_NOTEOL is never needed since the search window's end always
 * coincides with the subject's own true end (the window is a suffix,
 * never truncated on the right), so `$` already means the real end.
 *
 * Zero-width matches (e.g. "x*" can always match an empty string) are
 * real and must not infinite-loop: after recording one, `pos` advances
 * to `rm_eo + 1` (one past the empty match) rather than `rm_eo` (which
 * would just find the exact same empty match again at the same spot
 * forever) -- the skipped byte still ends up correctly included in the
 * next iteration's literal-text span, since that span is always
 * computed from `last_end` (which only moves to an actual match
 * boundary), not from `pos` itself.
 *
 * Verified against real nix-instantiate for the trickiest cases before
 * trusting this algorithm (see this function's own trace-through in the
 * commit/PR description for the full by-hand simulation): `split "x*"
 * "ab"` == `[ "" [] "a" [] "b" [] "" ]`; `split "-" "a-b-c"` == `[ "a" []
 * "b" [] "c" ]`; `split "(-)" "a-b-c"` == `[ "a" ["-"] "b" ["-"] "c" ]`;
 * `split "" ""` == `[ "" [] "" ]`; `split "a" ""` == `[ "" ]` (regexec
 * finds no match at all in an empty subject against a pattern that
 * requires a literal char, so the whole loop never fires and the single
 * trailing-text element is just the (empty) subject); `split ",*"
 * "a,,b"` == `[ "" [] "a" [] "" [] "b" [] "" ]`; `split "(a)(b)?" "aab"`
 * == `[ "" ["a" null] "" ["a" "b"] "" ]`.
 * --------------------------------------------------------------------- */
/* The actual split loop, entirely custom-engine-driven: `cre` already
 * compiled, `subject`/`slen` the full subject. Shared by both of
 * split2_impl's custom-engine routes below (the bracket-extension
 * bypass, and the "glibc's own split boundaries can't be trusted here
 * either" route right below). Does NOT free `cre` or `subject` --
 * callers own those. */
static Value split_via_custom_engine(const AsnixCustomRegex *cre, int64_t nsub, const char *subject, int64_t slen) {
    Value *out = NULL;
    int64_t cap = 0, n = 0;
    int64_t pos = 0, last_end = 0;
    AsnixRegexGroup *g = nsub > 0 ? malloc((size_t)nsub * sizeof *g) : NULL;

    while (pos <= slen) {
        int64_t abs_so, abs_eo;
        if (!asnix_regex_search(cre, subject, slen, pos, pos > 0, &abs_so, &abs_eo, g)) break;

        if (n + 2 > cap) {
            cap = cap ? cap * 2 : 8;
            out = realloc(out, (size_t)cap * sizeof(Value));
        }
        out[n++] = asnix_make_string_value(subject + last_end, (size_t)(abs_so - last_end));
        out[n++] = custom_groups_to_list(g, nsub, subject);

        last_end = abs_eo;
        pos = (abs_eo == abs_so) ? abs_eo + 1 : abs_eo;
    }
    free(g);

    if (n + 1 > cap) {
        cap = cap ? cap + 1 : 1;
        out = realloc(out, (size_t)cap * sizeof(Value));
    }
    out[n++] = asnix_make_string_value(subject + last_end, (size_t)(slen - last_end));

    Value result = asnix_bshared_make_list(out, n);
    free(out);
    return result;
}

static Value split2_impl(const Value *args, int64_t argc) {
    (void)argc;
    int64_t plen, slen;
    const char *pbytes, *sbytes;
    asnix_bshared_string_bytes(args[0], &pbytes, &plen, "split");
    asnix_bshared_string_bytes(args[1], &sbytes, &slen, "split");

    char *pattern = nul_copy(pbytes, plen);
    char *subject = nul_copy(sbytes, slen);

    /* M143: see asnix_prim_match2_impl's own comment -- resolve this
     * pattern's cache entry once, up front. */
    RegexCacheEntry *re = regex_cache_get(pattern, plen);

    /* Bracket-expression extension glibc can't be trusted for at all --
     * full custom-engine bypass, same as match2_impl's. */
    if (re->needs_custom) {
        if (re->rejected) {
            asnix_bshared_fatal("split", "invalid regular expression (rejected by real Nix's stricter ERE grammar)");
        }
        AsnixCustomRegex *cre = regex_cache_ensure_custom(re, pattern, plen);
        if (!cre) asnix_bshared_fatal("split", "invalid regular expression");
        free(pattern);
        Value result = split_via_custom_engine(cre, re->custom_nsub, subject, slen);
        free(subject);
        return result;
    }

    /* Still compile+validate via glibc regardless of which engine ends
     * up actually finding matches below -- regex_cache_ensure_glibc is
     * also where pattern_rejected_by_real_nix and regcomp's own syntax
     * rejections (Class C) happen, unchanged. */
    regex_t *preg = regex_cache_ensure_glibc(re, pattern, "split");
    size_t nsub = preg->re_nsub;

    /* Unlike match2_impl (anchored to the whole string, so regexec's
     * own leftmost-longest WHOLE-match-finding -- which is correct --
     * is all it relies on), split's matches are unanchored, and for
     * patterns with a greedy repetition immediately followed by
     * something optional (e.g. `(x*)(xy)?`), glibc's regexec and real
     * Nix's libstdc++ regex_search don't even agree on each match's
     * SPAN, not just its capture groups: glibc (true POSIX) finds the
     * longest span achievable by also backtracking the repetition's
     * own count, while libstdc++ commits to the repetition's greedy
     * count and never revisits it just because a later optional part
     * could have matched more -- confirmed directly, `split "(x*)(xy)?"
     * "xy"` gives 3 matches on real Nix ("x", then two zero-width ones)
     * but glibc's regexec alone finds one 2-char match ("xy"). So
     * whenever there's any capture group at all (nsub == 0 means
     * nothing could ever surface this either way) and the subject is
     * within the custom engine's safe recursion-depth budget (see
     * ASNIX_REGEX_CUSTOM_MAX_SPAN), route the ENTIRE search -- not just
     * capture recomputation -- through the custom engine, ignoring
     * glibc's regexec results for this pattern entirely (regcomp's
     * compile-time validation above still applies, this only skips
     * *executing* it). Falls back to the old glibc-driven loop
     * (captures still recomputed per match, as before) if the custom
     * engine can't parse the pattern or the subject is too long to
     * safely recurse over -- same defensive reasoning as
     * groups_to_list_recomputed. */
    if (nsub > 0 && slen <= ASNIX_REGEX_CUSTOM_MAX_SPAN) {
        AsnixCustomRegex *cre = regex_cache_ensure_custom(re, pattern, plen);
        if (cre && re->custom_nsub == (int64_t)nsub) {
            Value result = split_via_custom_engine(cre, re->custom_nsub, subject, slen);
            free(pattern);
            free(subject);
            return result;
        }
    }

    Value *out = NULL;
    int64_t cap = 0, n = 0;
    int64_t pos = 0, last_end = 0;
    regmatch_t *pmatch = malloc((nsub + 1) * sizeof(regmatch_t));

    while (pos <= slen) {
        int eflags = pos > 0 ? REG_NOTBOL : 0;
        int mrc = regexec(preg, subject + pos, nsub + 1, pmatch, eflags);
        if (mrc != 0) break;

        int64_t abs_so = pos + pmatch[0].rm_so;
        int64_t abs_eo = pos + pmatch[0].rm_eo;

        if (n + 2 > cap) {
            cap = cap ? cap * 2 : 8;
            out = realloc(out, (size_t)cap * sizeof(Value));
        }
        out[n++] = asnix_make_string_value(subject + last_end, (size_t)(abs_so - last_end));
        out[n++] = groups_to_list_recomputed(pmatch, nsub, subject + pos, re, pattern, plen, subject, slen,
                                              abs_so, abs_eo, pos > 0);

        last_end = abs_eo;
        pos = (abs_eo == abs_so) ? abs_eo + 1 : abs_eo;
    }

    if (n + 1 > cap) {
        cap = cap ? cap + 1 : 1;
        out = realloc(out, (size_t)cap * sizeof(Value));
    }
    out[n++] = asnix_make_string_value(subject + last_end, (size_t)(slen - last_end));

    Value result = asnix_bshared_make_list(out, n);
    free(out);
    free(pattern);
    free(pmatch);
    free(subject);
    return result;
}
static Value build_split(void) { return asnix_bshared_make_curried_native((void *)split2_impl, 2); }

const AsnixBuiltinMiscDef ASNIX_REGEX_BUILTINS[] = {
    { "match", build_match },
    { "split", build_split },
};

const size_t ASNIX_REGEX_BUILTINS_COUNT = sizeof(ASNIX_REGEX_BUILTINS) / sizeof(ASNIX_REGEX_BUILTINS[0]);
