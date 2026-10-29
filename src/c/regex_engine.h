/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_REGEX_ENGINE_H
#define LL_REGEX_ENGINE_H

#include <stdint.h>

/*
 * A small POSIX-ERE backtracking matcher -- see regex_engine.c's
 * header comment for the full rationale (glibc's <regex.h>, which
 * builtins_regex.c otherwise drives, disagrees with real Nix's
 * libstdc++ `std::regex` on *which* capture a repeated/ambiguous
 * subexpression gets, and cannot compile a handful of bracket-expression
 * extensions libstdc++ accepts at all). Used by builtins_regex.c in two
 * ways: (1) to recompute capture groups, matching libstdc++'s specific
 * convention, for a match span glibc's regexec already correctly found
 * (the common case, whenever the pattern has any capture group at all);
 * (2) as a full standalone replacement for regcomp/regexec on patterns
 * `ll_regex_needs_custom_engine` flags glibc can't be trusted for at
 * all.
 */

typedef struct LlCustomRegex LlCustomRegex;

/* A single capture group's result, as an ABSOLUTE byte offset pair into
 * whatever subject buffer the match call was given -- so < 0 means "did
 * not participate" (-> null to the Nix-level caller), the same
 * convention regexec's own rm_so == -1 already uses. */
typedef struct {
    int64_t so;
    int64_t eo;
} LlRegexGroup;

/* Bracket-expression-aware pre-scan: true iff `pattern` uses a construct
 * glibc's regcomp/regexec cannot be trusted for at all -- a
 * multi-character named collating symbol (`[.hyphen.]`), an equivalence
 * class (`[=a=]`), or a non-standard-POSIX character class name (a case
 * variant like `[:ALPHA:]`, or the GNU `[:w:]` extension). Patterns for
 * which this returns 0 are handled by the existing glibc path exactly as
 * before (this function does not duplicate `pattern_rejected_by_real_nix`'s
 * job -- a pattern can be both "not rejected" and "needs the custom
 * engine", that's the whole point). */
int ll_regex_needs_custom_engine(const char *pattern, int64_t len);

/* Parses `pattern` (the POSIX ERE subset documented at the top of
 * regex_engine.c) into a reusable compiled form. Returns NULL if the
 * pattern doesn't parse under this engine's own (deliberately narrower
 * than glibc's) grammar -- callers that still have glibc's own result
 * available should fall back to it rather than treat this as fatal (see
 * builtins_regex.c); callers on the needs-custom-engine path (where no
 * glibc result exists to fall back to) should treat it the same as
 * compile_or_fatal's regcomp failure. *nsub_out receives the number of
 * capture groups (same meaning as regex_t's re_nsub). */
LlCustomRegex *ll_regex_compile(const char *pattern, int64_t len, int64_t *nsub_out);

void ll_regex_free(LlCustomRegex *re);

/* Anchored match: does `re` match `subject[start, end)`, consuming
 * exactly that span, no more, no less (conceptually `regexec` on a
 * substring copy, but without actually copying -- `start`/`end` are
 * absolute offsets into the real, full-length `subject`, so `^`/`$`
 * still mean the true start/end of the whole string, not of this one
 * span). On success, fills `groups_out` (caller-allocated, nsub entries)
 * with each group's own absolute span and returns 1; on failure returns
 * 0 and leaves `groups_out` untouched. `notbol` mirrors REG_NOTBOL. */
int ll_regex_match_anchored(const LlCustomRegex *re, const char *subject, int64_t slen,
                                int64_t start, int64_t end, int notbol, LlRegexGroup *groups_out);

/* From-scratch leftmost-longest search (used only on the
 * needs-custom-engine path, where glibc is bypassed entirely): finds the
 * earliest start position >= from_pos with any match, and -- among
 * matches sharing that start -- the longest one (same convention
 * regexec itself follows, just reimplemented). Returns 1 and fills
 * *out_start/*out_end (absolute) plus groups_out (may be NULL if the
 * caller doesn't need captures) on success, 0 if no match exists
 * anywhere in subject[from_pos..slen]. `notbol` mirrors REG_NOTBOL
 * (split's windowed re-search from pos > 0). */
int ll_regex_search(const LlCustomRegex *re, const char *subject, int64_t slen,
                        int64_t from_pos, int notbol, int64_t *out_start, int64_t *out_end,
                        LlRegexGroup *groups_out);

#endif /* LL_REGEX_ENGINE_H */
