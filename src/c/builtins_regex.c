/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
/*
 * builtins.match / builtins.split -- see builtins_regex.h.
 *
 * regex_engine.c is a small hand-written backtracking ERE matcher used
 * alongside the glibc-based matching below -- see its header comment for
 * the full rationale: glibc's regexec and real Nix's libstdc++
 * `std::regex` disagree on which capture a repeated/ambiguous
 * subexpression gets, and glibc can't compile a few bracket-expression
 * extensions libstdc++ accepts. Routing, in both match2_impl and
 * split2_impl below: a pattern with zero capture groups never touches
 * the custom engine at all; a pattern using a bracket-expression
 * extension bypasses glibc entirely (`ll_regex_needs_custom_engine`);
 * everything else still uses glibc's regexec to find the match's overall
 * span and only asks the custom engine to recompute that span's capture
 * groups (`groups_to_list_recomputed`), falling back to glibc's own group
 * results unchanged if the custom engine can't parse the pattern at all.
 *
 * `ll_prim_match2_impl` additionally makes the custom engine
 * AUTHORITATIVE for the match/no-match decision itself once a given
 * pattern's custom-engine existence verdict has been empirically proven
 * to agree with glibc's -- see RegexCacheEntry's `span_trusted` field.
 * This is safe because `match`'s own question ("does the WHOLE
 * fixed-length string match") is answered correctly by ANY complete
 * backtracking search regardless of which decomposition it prefers when
 * more than one exists -- the capture ambiguity above only affects WHICH
 * capture wins, never whether a full-length match exists at all.
 * `split2_impl` is deliberately NOT given this same treatment: its
 * unanchored search can genuinely disagree with glibc on the match SPAN
 * itself even without capture groups (top-level alternation's
 * longest-vs-ordered preference can differ between libstdc++ and glibc
 * in search mode), a fundamentally different and harder-to-bound risk
 * than match's tie-break-independent existence question.
 */
#include "builtins_regex.h"

#include <regex.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ll/value.h"
#include "builtins_shared.h"
#include "regex_engine.h"
#include "value_util.h"

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
 * a GNU extension/leniency. Without this pre-check, `regcomp` alone
 * would accept several of them, so e.g. `builtins.match "\\n" "x"` would
 * wrongly succeed/return `null` here instead of being the fatal error
 * real Nix gives.
 *
 * - Escaping anything other than the 12 real POSIX ERE metacharacters
 *   (`. ( ) * + ? [ { ^ $ | \`) is rejected -- this is the single
 *   biggest category: `\n` `\w` `\d` `\b` `\/` (GNU-extension-only
 *   shorthand escapes glibc accepts but real Nix rejects) and, perhaps
 *   surprisingly, `\]` and `\}` too (asymmetric vs. valid `\(` `\)` `\[`
 *   `\{`, but that's genuinely what real Nix does, since `]` and `}`
 *   aren't "special" on their own in POSIX ERE grammar, only in context
 *   after a matching `[`/`{`). A backreference like `\1` falls under
 *   this same rule (a digit isn't in the 12-char list).
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
 * backslash has no special meaning inside `[...]` in POSIX ERE).
 *
 * `[[.-.]]`/`[a-[.c.]]`-shaped obscure POSIX collating-symbol forms
 * (non-alphabetic single-char collating names, and any collating symbol
 * used as a range's HIGH endpoint) are handled by routing every
 * `[.name.]` occurrence to the custom engine instead of trusting glibc's
 * more lenient `regcomp` for it -- see regex_engine.c's
 * `collate_lookup`/`parse_range_endpoint`/`ll_regex_needs_custom_engine`.
 *
 * `(a){24998}`-shaped group repetition: real Nix rejects large
 * group-repetition counts as "memory limit exceeded" at libstdc++'s own
 * internal NFA-state-count limit (~24998, confirmed empirically), well
 * below glibc's own much higher limit, which would otherwise accept it
 * (unlike a BARE repeated literal like `a{99999}`, where glibc's own
 * regcomp limit already rejects it for unrelated reasons). Since the
 * exact boundary depends on the repeated body's own shape and can't be
 * derived from a handful of probe patterns, `group_repeat_bound_too_large`
 * below instead rejects any `(...)` capture group followed by a
 * `{m}`/`{m,}`/`{m,n}` interval whose bound exceeds
 * `LL_GROUP_REPEAT_SAFE_MAX` (1000) -- comfortably above any
 * group-repetition count anywhere in real usage and comfortably below
 * the real ~24998 limit, so this can never misclassify anything
 * currently exercised; it is a conservative approximation of real Nix's
 * actual limit, not a faithful reproduction of it. */
#define LL_GROUP_REPEAT_SAFE_MAX 1000

/* `p[i] == '{'` on entry (caller already checked). Parses a POSIX ERE
 * interval `{m}`/`{m,}`/`{m,n}` starting there and returns 1 if either
 * bound present exceeds LL_GROUP_REPEAT_SAFE_MAX -- see the comment
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
    return (bound1 > LL_GROUP_REPEAT_SAFE_MAX) || (bound2 > LL_GROUP_REPEAT_SAFE_MAX);
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
                /* A just-closed group immediately followed by a large
                 * `{m}`/`{m,}`/`{m,n}` interval -- see
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

/* A pattern-string-keyed cache for both this file's compiled regex
 * forms -- glibc's `regex_t` (via `regcomp`) and regex_engine.c's own
 * `LlCustomRegex`. Entries are never evicted or freed for the life of
 * the process; a parallel open-addressed, linear-probe index array maps
 * each pattern's hash to its slot in `g_regex_cache`, grown/rehashed at
 * a 75% load factor before each insert. */
typedef struct {
    char *pattern;   /* owned copy, exactly `len` bytes, no NUL terminator */
    int64_t len;

    int rejected;      /* pattern_rejected_by_real_nix(pattern, len), computed once */
    int needs_custom;   /* ll_regex_needs_custom_engine(pattern, len), computed once */

    int have_glibc;       /* glibc_re is a successfully regcomp'd pattern */
    int glibc_bad;         /* regcomp itself failed -- glibc_err is the message to replay */
    char *glibc_err;        /* owned (regerror's own buffer is caller-provided/transient) */
    regex_t glibc_re;

    int custom_attempted;     /* ll_regex_compile already tried once (success or not) */
    LlCustomRegex *custom_re; /* compiled form, or NULL if the attempt failed */
    int64_t custom_nsub;

    /* Per-pattern empirical trust state for making the custom engine
     * AUTHORITATIVE for `match`'s whole-string span decision itself (not
     * just capture recompute for a span glibc already found). Tri-state:
     *   0  -- still verifying: the next several calls with this exact
     *         pattern cross-check the custom engine's existence verdict
     *         against glibc's regexec() on the SAME call, and the
     *         glibc-derived result (already correct, unchanged) is what
     *         gets returned while still in this state.
     *   1  -- trusted: `span_verify_count` consecutive calls all agreed,
     *         so `ll_prim_match2_impl` skips regexec() entirely for
     *         later calls with this pattern and trusts the custom
     *         engine's own existence verdict directly.
     *  -1  -- proven unsafe (a disagreement was actually observed, or
     *         the custom engine can't compile this pattern / reports a
     *         different group count than regcomp) -- permanently falls
     *         back to the regexec()-driven path for this pattern. */
    int span_trusted;
    int span_verify_count;
} RegexCacheEntry;

static RegexCacheEntry **g_regex_cache = NULL;
static size_t g_regex_cache_count = 0, g_regex_cache_cap = 0;
static size_t *g_regex_cache_index = NULL;
static size_t g_regex_cache_index_cap = 0;
#define REGEX_CACHE_EMPTY_SLOT ((size_t)-1)

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
    e->needs_custom = ll_regex_needs_custom_engine(pattern, len);
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
 * behavior (not `null`).
 *
 * `pattern` must still be the caller's own NUL-terminated scratch copy
 * (`e->pattern` is NOT NUL-terminated -- see above), since `regcomp`
 * needs a C string. */
static regex_t *regex_cache_ensure_glibc(RegexCacheEntry *e, const char *pattern, const char *who) {
    if (e->rejected) {
        ll_bshared_fatal(who, "invalid regular expression (rejected by real Nix's stricter ERE grammar)");
    }
    if (e->glibc_bad) {
        ll_bshared_fatal(who, e->glibc_err);
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
        ll_bshared_fatal(who, e->glibc_err);
    }
    e->have_glibc = 1;
    return &e->glibc_re;
}

/* Cache-aware wrapper around regex_engine.c's `ll_regex_compile` --
 * compiled at most once per distinct pattern string, reused by every
 * later call. */
static LlCustomRegex *regex_cache_ensure_custom(RegexCacheEntry *e, const char *pattern, int64_t plen) {
    if (e->custom_attempted) return e->custom_re;
    e->custom_attempted = 1;
    e->custom_re = ll_regex_compile(pattern, plen, &e->custom_nsub);
    return e->custom_re;
}

/* Builds the flat list of `nsub` capture-group values from a completed
 * regexec()'s pmatch[] array (pmatch[0] is the whole match, pmatch[1..nsub]
 * are the groups -- same indexing regexec itself uses). `base` is the
 * pointer regexec was actually called against (subject, or subject+pos
 * for split's windowed search below) -- pmatch offsets are relative to
 * whatever pointer regexec saw, not necessarily the start of the whole
 * subject. A group that didn't participate in the match (rm_so == -1)
 * becomes `null` in the list, not an empty string. */
static Value groups_to_list(const regmatch_t *pmatch, size_t nsub, const char *base) {
    Value *vals = nsub > 0 ? malloc(nsub * sizeof(Value)) : NULL;
    for (size_t i = 0; i < nsub; i++) {
        regmatch_t m = pmatch[i + 1];
        if (m.rm_so < 0) {
            vals[i] = LL_NULL;
        } else {
            vals[i] = ll_make_string_value(base + m.rm_so, (size_t)(m.rm_eo - m.rm_so));
        }
    }
    Value result = ll_bshared_make_list(vals, (int64_t)nsub);
    free(vals);
    return result;
}

/* Same shape as groups_to_list, but from the custom engine's own
 * LlRegexGroup[] (always absolute offsets into `subject`, never
 * relative to a search window -- regex_engine.c never slices/copies the
 * subject, see its header comment) rather than regexec's pmatch[]. */
static Value custom_groups_to_list(const LlRegexGroup *g, int64_t nsub, const char *subject) {
    Value *vals = nsub > 0 ? malloc((size_t)nsub * sizeof(Value)) : NULL;
    for (int64_t i = 0; i < nsub; i++) {
        if (g[i].so < 0) {
            vals[i] = LL_NULL;
        } else {
            vals[i] = ll_make_string_value(subject + g[i].so, (size_t)(g[i].eo - g[i].so));
        }
    }
    Value result = ll_bshared_make_list(vals, nsub);
    free(vals);
    return result;
}

/* regex_engine.c's matcher is plain recursive-descent-over-the-AST CPS
 * backtracking (see its header comment) -- a `*`/`+`/`{m,}` repeated
 * over N real (non-empty) iterations costs O(N) *C call stack* depth,
 * not just O(N) time, since each iteration recurses into the next
 * rather than looping. This segfaults for sufficiently long matched
 * spans (measured starting somewhere in the low thousands of matched
 * characters, sooner for nested-group patterns like
 * `((a|b)(c|d))+`). A real fix (converting repetition to an explicit
 * heap-allocated backtracking stack instead of C recursion) is out of
 * scope here; this cap is the deliberate mitigation instead, well below
 * the measured crash threshold. Above this cap,
 * `groups_to_list_recomputed` falls back to glibc's own (unimproved, but
 * also unregressed) `pmatch`-based capture groups. */
#define LL_REGEX_CUSTOM_MAX_SPAN 2000

/* Number of consecutive agreeing calls (per distinct pattern) required
 * before `ll_prim_match2_impl` trusts the custom engine's own existence
 * verdict and skips regexec() entirely for later calls -- see
 * RegexCacheEntry's `span_trusted` field. More than one call guards
 * against a single subject happening to agree by chance. */
#define LL_REGEX_SPAN_VERIFY_CALLS 8

/* Given a match glibc's regexec already found spans `[match_so,
 * match_eo)` of the full `subject` (absolute offsets -- `match_so`/
 * `match_eo` are NOT relative to `base`, see groups_to_list's own
 * comment on what `base` means for split's windowed calls), recomputes
 * that match's capture groups with the custom engine instead of trusting
 * regexec's own `pmatch[1..nsub]`. `notbol` mirrors REG_NOTBOL (split's
 * windowed re-search from pos > 0 passes 1).
 *
 * Falls back to groups_to_list's plain (glibc) result, unchanged, if the
 * match span is too long to safely recurse over (see
 * LL_REGEX_CUSTOM_MAX_SPAN above) or if the custom engine can't parse
 * `pattern` at all or reports a different group count than regcomp did
 * -- falling back defensively means a gap in the custom parser just
 * leaves the affected pattern exactly as correct/incorrect as the
 * glibc-only path already was, never introduces a new regression.
 *
 * `e` is the pattern's cache entry already resolved via
 * `regex_cache_get` -- the custom-engine form is compiled at most once
 * per distinct pattern, not once per match. */
static Value groups_to_list_recomputed(const regmatch_t *pmatch, size_t nsub, const char *base,
                                        RegexCacheEntry *e, const char *pattern, int64_t plen,
                                        const char *subject, int64_t slen, int64_t match_so,
                                        int64_t match_eo, int notbol) {
    if (nsub == 0) return groups_to_list(pmatch, nsub, base);
    if (match_eo - match_so > LL_REGEX_CUSTOM_MAX_SPAN) return groups_to_list(pmatch, nsub, base);
    LlCustomRegex *cre = regex_cache_ensure_custom(e, pattern, plen);
    if (cre && e->custom_nsub == (int64_t)nsub) {
        LlRegexGroup *g = malloc(nsub * sizeof *g);
        int ok = ll_regex_match_anchored(cre, subject, slen, match_so, match_eo, notbol, g);
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
 * way. This is exactly `std::regex_match`'s own semantics, just
 * reimplemented against POSIX regexec() instead of libstdc++'s regex
 * engine.
 *
 * No REG_NEWLINE is passed, matching real Nix's own regex construction
 * (which doesn't set multiline/dotall-excluding flags either) -- `.`
 * must match a literal newline too.
 * --------------------------------------------------------------------- */
Value ll_prim_match2_impl(const Value *args, int64_t argc) {
    (void)argc;
    int64_t plen, slen;
    const char *pbytes, *sbytes;
    ll_bshared_string_bytes(args[0], &pbytes, &plen, "match");
    ll_bshared_string_bytes(args[1], &sbytes, &slen, "match");

    char *pattern = nul_copy(pbytes, plen);
    char *subject = nul_copy(sbytes, slen);

    Value result;

    RegexCacheEntry *re = regex_cache_get(pattern, plen);

    /* Bracket-expression extension (named collating symbol, equivalence
     * class, or non-standard class name) glibc can't be trusted for at
     * all -- bypass it entirely, same leftmost-longest-anchoring trick
     * as the glibc path below, just driven by the custom engine's own
     * from-scratch search instead of regexec. */
    if (re->needs_custom) {
        if (re->rejected) {
            ll_bshared_fatal("match", "invalid regular expression (rejected by real Nix's stricter ERE grammar)");
        }
        LlCustomRegex *cre = regex_cache_ensure_custom(re, pattern, plen);
        if (!cre) ll_bshared_fatal("match", "invalid regular expression");
        free(pattern);
        int64_t nsub = re->custom_nsub;

        LlRegexGroup *g = nsub > 0 ? malloc((size_t)nsub * sizeof *g) : NULL;
        int64_t mso, meo;
        int found = ll_regex_search(cre, subject, slen, 0, 0, &mso, &meo, g);
        if (!found || mso != 0 || meo != slen) {
            result = LL_NULL;
        } else {
            result = custom_groups_to_list(g, nsub, subject);
        }
        free(g);
        free(subject);
        return result;
    }

    regex_t *preg = regex_cache_ensure_glibc(re, pattern, "match");

    size_t nsub = preg->re_nsub;

    /* Once this exact pattern's custom-engine existence verdict has been
     * empirically proven to agree with glibc's regexec() (see
     * RegexCacheEntry's `span_trusted` comment above), skip regexec()
     * entirely. Falls through to the glibc-driven path below (unchanged)
     * whenever not yet trusted, proven unsafe, the custom engine can't
     * compile this pattern, or the subject is too long to safely
     * recurse over. */
    if (re->span_trusted == 1 && slen <= LL_REGEX_CUSTOM_MAX_SPAN) {
        LlCustomRegex *cre = regex_cache_ensure_custom(re, pattern, plen);
        if (cre && re->custom_nsub == (int64_t)nsub) {
            LlRegexGroup *g = nsub > 0 ? malloc(nsub * sizeof *g) : NULL;
            int ok = ll_regex_match_anchored(cre, subject, slen, 0, slen, 0, g);
            if (ok) {
                result = nsub > 0 ? custom_groups_to_list(g, (int64_t)nsub, subject) : ll_bshared_make_list(NULL, 0);
            } else {
                result = LL_NULL;
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

    /* Still in the verification window for this pattern -- cross-check
     * the custom engine's own existence verdict against glibc's (just
     * computed above) on this SAME call/subject, without changing what
     * this call itself returns (the glibc-derived result below is used
     * regardless). */
    if (re->span_trusted == 0 && slen <= LL_REGEX_CUSTOM_MAX_SPAN) {
        LlCustomRegex *cre = regex_cache_ensure_custom(re, pattern, plen);
        if (cre && re->custom_nsub == (int64_t)nsub) {
            LlRegexGroup *g = nsub > 0 ? malloc(nsub * sizeof *g) : NULL;
            int custom_ok = ll_regex_match_anchored(cre, subject, slen, 0, slen, 0, g);
            if (custom_ok != glibc_ok) {
                re->span_trusted = -1; /* diverged for real -- never trust this pattern's span */
            } else {
                re->span_verify_count++;
                if (re->span_verify_count >= LL_REGEX_SPAN_VERIFY_CALLS) re->span_trusted = 1;
            }
            free(g);
        } else {
            re->span_trusted = -1; /* can't compile under the custom engine, or a differing group count */
        }
    }

    if (!glibc_ok) {
        result = LL_NULL;
    } else {
        result = groups_to_list_recomputed(pmatch, nsub, subject, re, pattern, plen, subject, slen, 0, slen, 0);
    }

    free(pattern);
    free(pmatch);
    free(subject);
    return result;
}
static Value build_match(void) { return ll_bshared_make_curried_native((void *)ll_prim_match2_impl, 2); }

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
 * the search window happens to start -- REG_NOTEOL is never needed
 * since the search window's end always coincides with the subject's own
 * true end, so `$` already means the real end.
 *
 * Zero-width matches (e.g. "x*" can always match an empty string) are
 * real and must not infinite-loop: after recording one, `pos` advances
 * to `rm_eo + 1` (one past the empty match) rather than `rm_eo` (which
 * would just find the exact same empty match again at the same spot
 * forever) -- the skipped byte still ends up correctly included in the
 * next iteration's literal-text span, since that span is always
 * computed from `last_end` (which only moves to an actual match
 * boundary), not from `pos` itself.
 * --------------------------------------------------------------------- */
/* The actual split loop, entirely custom-engine-driven: `cre` already
 * compiled, `subject`/`slen` the full subject. Does NOT free `cre` or
 * `subject` -- callers own those. */
static Value split_via_custom_engine(const LlCustomRegex *cre, int64_t nsub, const char *subject, int64_t slen) {
    Value *out = NULL;
    int64_t cap = 0, n = 0;
    int64_t pos = 0, last_end = 0;
    LlRegexGroup *g = nsub > 0 ? malloc((size_t)nsub * sizeof *g) : NULL;

    while (pos <= slen) {
        int64_t abs_so, abs_eo;
        if (!ll_regex_search(cre, subject, slen, pos, pos > 0, &abs_so, &abs_eo, g)) break;

        if (n + 2 > cap) {
            cap = cap ? cap * 2 : 8;
            out = realloc(out, (size_t)cap * sizeof(Value));
        }
        out[n++] = ll_make_string_value(subject + last_end, (size_t)(abs_so - last_end));
        out[n++] = custom_groups_to_list(g, nsub, subject);

        last_end = abs_eo;
        pos = (abs_eo == abs_so) ? abs_eo + 1 : abs_eo;
    }
    free(g);

    if (n + 1 > cap) {
        cap = cap ? cap + 1 : 1;
        out = realloc(out, (size_t)cap * sizeof(Value));
    }
    out[n++] = ll_make_string_value(subject + last_end, (size_t)(slen - last_end));

    Value result = ll_bshared_make_list(out, n);
    free(out);
    return result;
}

static Value split2_impl(const Value *args, int64_t argc) {
    (void)argc;
    int64_t plen, slen;
    const char *pbytes, *sbytes;
    ll_bshared_string_bytes(args[0], &pbytes, &plen, "split");
    ll_bshared_string_bytes(args[1], &sbytes, &slen, "split");

    char *pattern = nul_copy(pbytes, plen);
    char *subject = nul_copy(sbytes, slen);

    RegexCacheEntry *re = regex_cache_get(pattern, plen);

    /* Bracket-expression extension glibc can't be trusted for at all --
     * full custom-engine bypass, same as match2_impl's. */
    if (re->needs_custom) {
        if (re->rejected) {
            ll_bshared_fatal("split", "invalid regular expression (rejected by real Nix's stricter ERE grammar)");
        }
        LlCustomRegex *cre = regex_cache_ensure_custom(re, pattern, plen);
        if (!cre) ll_bshared_fatal("split", "invalid regular expression");
        free(pattern);
        Value result = split_via_custom_engine(cre, re->custom_nsub, subject, slen);
        free(subject);
        return result;
    }

    /* Still compile+validate via glibc regardless of which engine ends
     * up actually finding matches below -- regex_cache_ensure_glibc is
     * also where pattern_rejected_by_real_nix and regcomp's own syntax
     * rejections happen, unchanged. */
    regex_t *preg = regex_cache_ensure_glibc(re, pattern, "split");
    size_t nsub = preg->re_nsub;

    /* Unlike match2_impl (anchored to the whole string, so regexec's own
     * leftmost-longest WHOLE-match-finding is all it relies on), split's
     * matches are unanchored, and for patterns with a greedy repetition
     * immediately followed by something optional (e.g. `(x*)(xy)?`),
     * glibc's regexec and real Nix's libstdc++ regex_search don't even
     * agree on each match's SPAN, not just its capture groups: glibc
     * (true POSIX) finds the longest span achievable by also
     * backtracking the repetition's own count, while libstdc++ commits
     * to the repetition's greedy count and never revisits it just
     * because a later optional part could have matched more (e.g.
     * `split "(x*)(xy)?" "xy"` gives 3 matches on real Nix but glibc's
     * regexec alone finds one). So whenever there's any capture group at
     * all and the subject is within the custom engine's safe
     * recursion-depth budget (see LL_REGEX_CUSTOM_MAX_SPAN), route the
     * ENTIRE search -- not just capture recomputation -- through the
     * custom engine, ignoring glibc's regexec results for this pattern
     * entirely (regcomp's compile-time validation above still applies,
     * this only skips *executing* it). Falls back to the old
     * glibc-driven loop if the custom engine can't parse the pattern or
     * the subject is too long to safely recurse over. */
    if (nsub > 0 && slen <= LL_REGEX_CUSTOM_MAX_SPAN) {
        LlCustomRegex *cre = regex_cache_ensure_custom(re, pattern, plen);
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
        out[n++] = ll_make_string_value(subject + last_end, (size_t)(abs_so - last_end));
        out[n++] = groups_to_list_recomputed(pmatch, nsub, subject + pos, re, pattern, plen, subject, slen,
                                              abs_so, abs_eo, pos > 0);

        last_end = abs_eo;
        pos = (abs_eo == abs_so) ? abs_eo + 1 : abs_eo;
    }

    if (n + 1 > cap) {
        cap = cap ? cap + 1 : 1;
        out = realloc(out, (size_t)cap * sizeof(Value));
    }
    out[n++] = ll_make_string_value(subject + last_end, (size_t)(slen - last_end));

    Value result = ll_bshared_make_list(out, n);
    free(out);
    free(pattern);
    free(pmatch);
    free(subject);
    return result;
}
static Value build_split(void) { return ll_bshared_make_curried_native((void *)split2_impl, 2); }

const LlBuiltinMiscDef LL_REGEX_BUILTINS[] = {
    { "match", build_match },
    { "split", build_split },
};

const size_t LL_REGEX_BUILTINS_COUNT = sizeof(LL_REGEX_BUILTINS) / sizeof(LL_REGEX_BUILTINS[0]);
