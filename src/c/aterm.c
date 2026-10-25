/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#include "aterm.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *buf;
    size_t len, cap;
} Buf;

/* M174: `asnix_build_derivation_aterm_cached` used to start every single
 * call from a fresh `Buf b = { 0 }` (NULL, zero-capacity), growing it by
 * doubling via `realloc` as bytes were appended, then handing the final
 * `b.buf` straight to the caller to `free()`. derivation.c calls this up
 * to 3 times PER DERIVATION (masked/real/cache-hash passes, M151's own
 * comment above `aterm_cache`), and on the real d.nix/ffmpeg eval that is
 * 5,229 derivation builds -- 64,274 `realloc` calls inside `buf_reserve`
 * total, ~13,427 of them the very first cap-0-to-256 allocation, for a
 * fresh malloc-to-free lifecycle every single call. M151 already deduped
 * the redundant re-ESCAPING *work* across those 3 passes (the character-
 * by-character scan); it left this allocation/growth/free churn
 * completely untouched -- that's what this milestone fixes, the same
 * anti-pattern `iets` found and fixed in its own derivation-ATerm
 * serialization (replacing a fresh growing buffer per unparse with one
 * buffer per thread that keeps its capacity across calls).
 *
 * Fix: one process-global `Buf` (`g_aterm_buf` below) that keeps its
 * allocated capacity across EVERY call across the whole eval, reset
 * (`len = 0`, `cap` untouched) at the start of each call instead of
 * being freed and started fresh. Safe as a single global precisely
 * because asnix is single-threaded (see `bytecode_cache.h`'s own note:
 * the one place this project forks work out, it uses a forked PROCESS,
 * never a pthread) AND because this function is a pure string
 * serializer over already-fully-evaluated C strings -- it calls nothing
 * that evaluates Nix code or could call back into this function while a
 * call is already in progress, so there is no reentrancy/nesting case
 * where two live calls could want the same buffer at once (confirmed by
 * reading every function this file calls: `buf_reserve`/`buf_putc`/
 * `buf_put_bytes`/`buf_put_escaped`/`buf_put_escaped_raw`/
 * `find_escape_aterm`/the `AtermEscapeCache` lookup/insert helpers --
 * none of them touch the evaluator).
 *
 * The existing API contract still hands every caller a malloc'd,
 * independently-owned, NUL-terminated buffer it `free()`s itself
 * (`derivation.c`'s 3 call sites each `sha256_or_die` the result then
 * `free()` it immediately -- none holds on to it past the next call
 * into this function, confirmed by reading every call site, but the
 * contract is honored unconditionally regardless) -- so the final
 * `asnix_build_derivation_aterm_cached` step below copies the finished
 * text out of `g_aterm_buf` into a fresh, exactly-sized `malloc`
 * (ONE allocation, sized precisely, instead of ~12 doubling reallocs
 * worth of copy-on-grow churn) and returns THAT, leaving `g_aterm_buf`
 * itself untouched (still allocated, ready for the next call to reset
 * and reuse) rather than handing its own backing storage to the caller.
 * `AtermEscapeCache`'s cached byte spans are unaffected either way: a
 * cache entry's `val` is always ITS OWN separate `malloc`'d copy (see
 * `buf_put_escaped` below), copied out of `b->buf[start..b->len)` at
 * insert time, never a pointer into `b->buf` itself -- so it stays
 * correct whether `b->buf` is a fresh allocation every call (the old
 * behavior) or a long-lived buffer that gets `realloc`'d out from under
 * a previous call's pointer (this fix's new behavior); nothing outside
 * this file ever sees or holds a raw pointer into `g_aterm_buf`. */
static Buf g_aterm_buf = { 0 };

static void buf_reserve(Buf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    b->cap = b->cap ? b->cap * 2 : 256;
    while (b->len + extra + 1 > b->cap) b->cap *= 2;
    b->buf = realloc(b->buf, b->cap);
}

static void buf_putc(Buf *b, char c) {
    buf_reserve(b, 1);
    b->buf[b->len++] = c;
}

static void buf_put_bytes(Buf *b, const char *s, size_t n) {
    buf_reserve(b, n);
    memcpy(b->buf + b->len, s, n);
    b->len += n;
}

/* M154: every single call site below passes a C STRING LITERAL (the fixed
 * ATerm structural punctuation, plus the 5 two-byte escape sequences
 * \n/\r/\t/\"/\\ -- never a `derivation.c`-owned dynamic string; those all
 * go through `buf_put_escaped`/`buf_put_escaped_raw` instead). A literal's
 * length is known at compile time via `sizeof`, so there is nothing to
 * measure with a runtime `strlen()` call -- the previous `buf_puts(Buf*,
 * const char*)` helper strlen()'d its argument on every call, including
 * the 5 escape-sequence cases inside `buf_put_escaped_raw`'s hot loop
 * (one call per actual \n/\r/\t/"/\\ byte escaped -- e.g. every newline in
 * a multi-line `buildCommand`/env value). `lit` MUST be a string literal
 * (or other `char[N]`), not a `char *` variable -- `sizeof` on a pointer
 * would silently compute the wrong (pointer-sized) length instead of
 * failing loudly, so this is deliberately NOT a general-purpose replacement
 * for `buf_puts`. */
#define buf_put_lit(b, lit) buf_put_bytes((b), (lit), sizeof(lit) - 1)

/* perf (M138): SWAR scan for the aterm escape set (\n \r \t \" \\),
 * processing 8 bytes per iteration instead of one branchy compare-chain
 * per byte. Ported from iets's own `find_escape` (~/gnix/iets/src/store/
 * drv.rs) -- confirmed via its own exhaustive-fuzz unit test
 * (`find_escape_finds_the_first_byte_to_escape`) and re-derived here
 * against the two underlying bit-twiddling identities independently
 * (Sean Eron Anderson's "bit twiddling hacks": "determine if a word has
 * a byte less than n" -- correct for ANY byte value 0-255, not just
 * ASCII, as long as n<=128; and the companion "has a zero byte" trick
 * used here to detect an exact byte match via XOR-then-haszero). Pure
 * filter: a chunk flagged "maybe" is re-scanned byte-by-byte to find the
 * real position (handles the trick's rare false positives); a chunk
 * flagged "no" is trusted outright (the identities have no false
 * negatives), so output is byte-for-byte identical to a plain scalar
 * scan for every input, including non-ASCII/non-UTF-8 bytes. */
static size_t find_escape_aterm(const unsigned char *s, size_t len) {
    static const uint64_t LO = 0x0101010101010101ULL;
    static const uint64_t HI = 0x8080808080808080ULL;
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t x;
        memcpy(&x, s + i, 8);
        uint64_t q = x ^ (LO * (uint64_t)'"');
        uint64_t bs = x ^ (LO * (uint64_t)'\\');
        /* any byte < 0x0e (covers \t=0x09,\n=0x0a,\r=0x0d) OR any byte
         * == '"' OR any byte == '\\'. */
        uint64_t any = ((x - LO * 0x0e) & ~x) | ((q - LO) & ~q) | ((bs - LO) & ~bs);
        if (any & HI) {
            for (size_t j = i; j < i + 8; j++) {
                unsigned char c = s[j];
                if (c == '\n' || c == '\r' || c == '\t' || c == '"' || c == '\\') return j;
            }
        }
    }
    for (; i < len; i++) {
        unsigned char c = s[i];
        if (c == '\n' || c == '\r' || c == '\t' || c == '"' || c == '\\') return i;
    }
    return len;
}

/* M151: per-derivation escape cache, keyed by SOURCE POINTER (see aterm.h's
 * own comment on `AtermEscapeCache`/`asnix_build_derivation_aterm_cached`
 * for why pointer identity, not content, is both correct and exactly what
 * `derivation.c`'s 3-passes-per-derivation caller needs). Plain open-
 * addressing table, linear probe, power-of-two capacity -- the per-
 * derivation entry count is small (roughly 2*nenv + nargs + ninput_srcs +
 * a handful) and known well enough up front (the caller's own `hint`) that
 * a simple table comfortably outperforms anything fancier here. */
typedef struct {
    const char *key; /* NULL = empty slot */
    char *val;        /* malloc'd escaped bytes (quotes included), NOT NUL-terminated */
    size_t len;
} AtermCacheSlot;

struct AtermEscapeCache {
    AtermCacheSlot *slots;
    size_t cap;   /* power of two, > 0 */
    size_t count; /* occupied slots -- tracked incrementally, NOT rescanned
                    * per insert (an earlier version of this cache scanned
                    * the whole table on every single insert to recompute
                    * this, an O(cap) cost paid once per distinct string;
                    * measured directly, by itself, to be the dominant cost
                    * of this whole milestone's fix -- see M151's own
                    * writeup). */
};

static size_t aterm_ptr_hash(const char *p) {
    uint64_t x = (uint64_t)(uintptr_t)p;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return (size_t)x;
}

AtermEscapeCache *asnix_aterm_escape_cache_new(size_t hint) {
    size_t cap = 64;
    while (cap < hint * 2) cap *= 2;
    AtermEscapeCache *c = malloc(sizeof(*c));
    c->slots = calloc(cap, sizeof(AtermCacheSlot));
    c->cap = cap;
    c->count = 0;
    return c;
}

void asnix_aterm_escape_cache_free(AtermEscapeCache *c) {
    if (!c) return;
    for (size_t i = 0; i < c->cap; i++) free(c->slots[i].val);
    free(c->slots);
    free(c);
}

static int aterm_cache_lookup(AtermEscapeCache *c, const char *s, char **out_val, size_t *out_len) {
    size_t mask = c->cap - 1;
    size_t i = aterm_ptr_hash(s) & mask;
    for (;;) {
        if (c->slots[i].key == NULL) return 0;
        if (c->slots[i].key == s) {
            *out_val = c->slots[i].val;
            *out_len = c->slots[i].len;
            return 1;
        }
        i = (i + 1) & mask;
    }
}

/* Every string this cache ever sees comes from one bounded derivation
 * build (derivation.c's caller creates one cache, uses it for exactly the
 * masked/real/cache-hash passes of ONE derivation, then frees it) -- the
 * entry count is known in advance well enough (the `hint` passed to
 * `asnix_aterm_escape_cache_new`) that growing is a rare safety net, not
 * the common case; still handled correctly (rehash into a fresh, doubled
 * table) rather than assumed away. */
static void aterm_cache_grow(AtermEscapeCache *c) {
    size_t old_cap = c->cap;
    AtermCacheSlot *old_slots = c->slots;
    size_t new_cap = old_cap * 2;
    AtermCacheSlot *new_slots = calloc(new_cap, sizeof(AtermCacheSlot));
    size_t mask = new_cap - 1;
    for (size_t i = 0; i < old_cap; i++) {
        if (!old_slots[i].key) continue;
        size_t j = aterm_ptr_hash(old_slots[i].key) & mask;
        while (new_slots[j].key) j = (j + 1) & mask;
        new_slots[j] = old_slots[i];
    }
    free(old_slots);
    c->slots = new_slots;
    c->cap = new_cap;
}

static void aterm_cache_insert(AtermEscapeCache *c, const char *s, char *val, size_t len) {
    /* Keep load factor <= ~70% so linear-probe chains stay short. */
    if ((c->count + 1) * 10 > c->cap * 7) aterm_cache_grow(c);
    size_t mask = c->cap - 1;
    size_t i = aterm_ptr_hash(s) & mask;
    while (c->slots[i].key) i = (i + 1) & mask;
    c->slots[i].key = s;
    c->slots[i].val = val;
    c->slots[i].len = len;
    c->count++;
}

/* Nix's ATerm escaping: only \n \r \t \" \\ are ever escaped; every other
 * byte (including non-ASCII/non-UTF-8) is written verbatim. Includes the
 * surrounding quotes. Output is byte-for-byte identical to a plain
 * per-byte scan-and-switch loop. Appends directly to *b (does not reset
 * b->len) so a cache-miss caller can read back the exact span it just
 * wrote (b->buf[start..b->len)) without a separate scratch buffer. */
static void buf_put_escaped_raw(Buf *b, const char *s) {
    buf_putc(b, '"');
    size_t len = strlen(s);
    const unsigned char *p = (const unsigned char *)s;
    size_t pos = 0;
    for (;;) {
        size_t esc = pos + find_escape_aterm(p + pos, len - pos);
        if (esc != pos) buf_put_bytes(b, (const char *)(p + pos), esc - pos);
        if (esc == len) break;
        switch (p[esc]) {
            case '\n': buf_put_lit(b, "\\n"); break;
            case '\r': buf_put_lit(b, "\\r"); break;
            case '\t': buf_put_lit(b, "\\t"); break;
            case '"': buf_put_lit(b, "\\\""); break;
            case '\\': buf_put_lit(b, "\\\\"); break;
        }
        pos = esc + 1;
    }
    buf_putc(b, '"');
}

/* M151: `cache`-aware entry point -- NULL `cache` is exactly the pre-M151
 * behavior (escape straight into `b`, every call, no memoization). With a
 * cache, a pointer seen before (within THIS cache's lifetime) is a pure
 * `buf_put_bytes` memcpy, skipping `find_escape_aterm`'s scan and the
 * switch-based re-encode entirely; a first-seen pointer pays the normal
 * cost once, then has its exact output span (read back from `b`, since
 * `buf_put_escaped_raw` appends in place) copied into the cache for the
 * next pass to reuse. */
static void buf_put_escaped(Buf *b, const char *s, AtermEscapeCache *cache) {
    if (cache) {
        char *cval;
        size_t clen;
        if (aterm_cache_lookup(cache, s, &cval, &clen)) {
            buf_put_bytes(b, cval, clen);
            return;
        }
    }
    size_t start = b->len;
    buf_put_escaped_raw(b, s);
    if (cache) {
        size_t clen = b->len - start;
        char *cval = malloc(clen ? clen : 1);
        memcpy(cval, b->buf + start, clen);
        aterm_cache_insert(cache, s, cval, clen);
    }
}

char *asnix_build_derivation_aterm_cached(const AtermOutput *outputs, size_t noutputs,
                                           const AtermInputDrv *input_drvs, size_t ninput_drvs,
                                           const char **input_srcs, size_t ninput_srcs, const char *system,
                                           const char *builder, const char **args, size_t nargs,
                                           const AtermEnvVar *env, size_t nenv, AtermEscapeCache *cache) {
    Buf *bp = &g_aterm_buf;
    bp->len = 0; /* reuse the persistent allocation; capacity untouched */
    Buf b = *bp;

    buf_put_lit(&b, "Derive([");
    for (size_t i = 0; i < noutputs; i++) {
        if (i > 0) buf_putc(&b, ',');
        buf_putc(&b, '(');
        buf_put_escaped(&b, outputs[i].name, cache);
        buf_putc(&b, ',');
        buf_put_escaped(&b, outputs[i].path, cache);
        buf_putc(&b, ',');
        buf_put_escaped(&b, outputs[i].hash_algo ? outputs[i].hash_algo : "", cache);
        buf_putc(&b, ',');
        buf_put_escaped(&b, outputs[i].hash ? outputs[i].hash : "", cache);
        buf_putc(&b, ')');
    }
    buf_put_lit(&b, "],[");
    for (size_t i = 0; i < ninput_drvs; i++) {
        if (i > 0) buf_putc(&b, ',');
        buf_putc(&b, '(');
        buf_put_escaped(&b, input_drvs[i].drv_path, cache);
        buf_put_lit(&b, ",[");
        for (size_t j = 0; j < input_drvs[i].noutput_names; j++) {
            if (j > 0) buf_putc(&b, ',');
            buf_put_escaped(&b, input_drvs[i].output_names[j], cache);
        }
        buf_put_lit(&b, "])");
    }
    buf_put_lit(&b, "],[");
    for (size_t i = 0; i < ninput_srcs; i++) {
        if (i > 0) buf_putc(&b, ',');
        buf_put_escaped(&b, input_srcs[i], cache);
    }
    buf_put_lit(&b, "],");
    buf_put_escaped(&b, system, cache);
    buf_putc(&b, ',');
    buf_put_escaped(&b, builder, cache);
    buf_putc(&b, ',');

    buf_putc(&b, '[');
    for (size_t i = 0; i < nargs; i++) {
        if (i > 0) buf_putc(&b, ',');
        buf_put_escaped(&b, args[i], cache);
    }
    buf_put_lit(&b, "],[");
    for (size_t i = 0; i < nenv; i++) {
        if (i > 0) buf_putc(&b, ',');
        buf_putc(&b, '(');
        buf_put_escaped(&b, env[i].key, cache);
        buf_putc(&b, ',');
        buf_put_escaped(&b, env[i].value, cache);
        buf_putc(&b, ')');
    }
    buf_put_lit(&b, "])");

    buf_putc(&b, '\0'); /* buf_reserve always keeps +1 slack for this */

    /* Write the (possibly grown/reallocated) buffer state back into the
     * persistent slot before this function returns, so the NEXT call
     * reuses whatever capacity this call ended up needing -- `b` above
     * is a local-by-value copy of `*bp` (not an alias), so `buf_reserve`'s
     * mutations of `b.buf`/`b.cap` only land in `*bp` via this explicit
     * writeback, not automatically. */
    *bp = b;

    /* Copy out into a fresh, EXACTLY-sized allocation (one malloc, no
     * doubling/over-allocation) rather than returning `bp->buf` itself --
     * `bp->buf` must stay alive and owned by this file for the next
     * call to reuse; every existing caller still gets back a malloc'd,
     * independently-owned, NUL-terminated buffer it can `free()` on its
     * own schedule, exactly as before this milestone. */
    char *out = malloc(b.len);
    memcpy(out, b.buf, b.len);
    return out;
}

char *asnix_build_derivation_aterm(const AtermOutput *outputs, size_t noutputs,
                                    const AtermInputDrv *input_drvs, size_t ninput_drvs, const char **input_srcs,
                                    size_t ninput_srcs, const char *system, const char *builder, const char **args,
                                    size_t nargs, const AtermEnvVar *env, size_t nenv) {
    return asnix_build_derivation_aterm_cached(outputs, noutputs, input_drvs, ninput_drvs, input_srcs, ninput_srcs,
                                                system, builder, args, nargs, env, nenv, NULL);
}
