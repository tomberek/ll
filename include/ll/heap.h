/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_HEAP_H
#define LL_HEAP_H

/*
 * Heap object layouts for the LL_TAG_PTR (string, path), LL_TAG_LIST,
 * and LL_TAG_ATTRS value kinds (see docs/value-encoding.md). All arena-
 * allocated, never freed, 8-byte aligned.
 */

/* String: {int64_t len; char bytes[len];} -- `len` is always >= 0. */
#define LL_STRING_OFF_LEN   0
#define LL_STRING_OFF_BYTES 8

/* Path (import-paths): a second LL_TAG_PTR kind -- there's no free tag
 * bit to spend on a dedicated LL_TAG_PATH (all 8 slots are already
 * assigned, see value.h). Told apart from a String with a single extra
 * load: a genuine String's leading word is always its own (non-negative)
 * byte length, so a Path reserves the negative sentinel LL_PATH_MARKER
 * there instead, pushing its own length and bytes out one slot:
 *   Path{int64_t marker == LL_PATH_MARKER (-1); int64_t len; char bytes[len];}
 * `bytes` always holds a fully-resolved ABSOLUTE, lexically-canonicalized
 * path (no `.`/`..`/doubled slashes, no trailing slash) -- resolution
 * against the containing file's directory happens once, at compile time
 * (see src/c/compiler.c's AST_PATH case), not at every use. Use
 * ll_value_is_path()/ll_ptr_bytes() (src/c/value_util.h) rather than
 * poking these offsets directly -- they dispatch on the marker so callers
 * that don't care which PTR kind they have (printing, hashing, ...) don't
 * have to. */
#define LL_PATH_MARKER     (-1)
#define LL_PATH_OFF_MARKER 0
#define LL_PATH_OFF_LEN    8
#define LL_PATH_OFF_BYTES  16

/* Float: a THIRD `LL_TAG_PTR` kind, alongside String/Path above, using
 * the same "reserved negative marker in the leading word" scheme:
 *   Float{int64_t marker == LL_FLOAT_MARKER (-2); double value;}
 * Unlike Path, a double needs the full 64 bits, so this is a genuine
 * second word, not a repurposing. Use
 * ll_make_float_value()/ll_value_is_float()/ll_float_value()
 * (value_util.h) -- never poke these offsets directly. */
#define LL_FLOAT_MARKER     (-2)
#define LL_FLOAT_OFF_MARKER 0
#define LL_FLOAT_OFF_VALUE  8

/* Cell: a FOURTH `LL_TAG_PTR` kind, alongside String/Path/Float above --
 * same "reserved negative marker in the leading word" precedent, one
 * word further along:
 *   Cell{int64_t marker == LL_CELL_MARKER (-3); Value *target;}
 * A Cell is NOT a Nix-visible value kind -- it's a purely internal
 * evaluator device: instead of allocating a fresh Thunk for a
 * bare-variable sub-expression about to be stored somewhere lazily, a
 * Cell just wraps a pointer STRAIGHT AT the existing Env slot that
 * variable already lives in, so two independent occurrences of the same
 * bare variable end up *aliasing the same underlying storage* instead of
 * each getting its own, separately-forced copy.
 *
 * `target` points into an Env's `values[]` array (see env.h) -- always
 * arena-allocated, never freed, so the pointer stays valid for the
 * program's entire lifetime; dereferencing it yields another raw
 * (possibly still-unforced-Thunk, or even another Cell) Value, which
 * every force chokepoint (force.S, op_var_ref.S, op_select.S,
 * op_select_or.S, op_with.S's OP_WITH_TRY) must treat as "not yet a
 * usable value" exactly like a Thunk, recursively resolving through it.
 * Use ll_make_cell_value()/ll_value_is_cell()/ll_cell_target()
 * (src/c/value_util.c) -- never poke these offsets directly. */
#define LL_CELL_MARKER     (-3)
#define LL_CELL_OFF_MARKER 0
#define LL_CELL_OFF_TARGET 8

/* Generous above any legitimate chain of bare-variable aliases a real
 * program could write -- chosen to abort in well under a second even on
 * a genuine cycle (e.g. `let x = y; y = x; in x`). Shared between
 * value_util.c's ll_resolve_cell_chain (the out-of-line reference
 * implementation, still used by op_select.S/op_select_or.S/op_with.S)
 * and the hand-inlined copies of the same loop in op_var_ref.S/force.S
 * -- defined once here so the three copies can never drift apart on the
 * bound. */
#define LL_CELL_CHASE_LIMIT 1000000

/* LazyApp: a FIFTH `LL_TAG_PTR` kind, alongside String/Path/Float/Cell
 * above -- same "reserved negative marker in the leading word"
 * precedent, but this one has TWO possible marker values, used as a
 * two-state machine:
 *
 *   LazyApp, UNRESOLVED:
 *     { int64_t marker == LL_LAZYAPP_MARKER (-4);
 *       Value   fn; Value arg1; Value arg2; }
 *   LazyApp, RESOLVED (the SAME heap object, mutated in place once --
 *   see below):
 *     { int64_t marker == LL_LAZYAPP_FORCED_MARKER (-5);
 *       Value   result; <2 dead words, never read again>; }
 *
 * A LazyApp replaces an Env+Thunk pair for `map`/`mapAttrs`/
 * `zipAttrsWith`'s per-output-element application with ONE heap object
 * big enough to hold `fn` plus up to two arguments -- `fn arg1` (`map`'s
 * case; `arg2.raw == 0`, reusing the "raw 0 is never a legal Value"
 * sentinel) or the curried `(fn arg1) arg2`
 * (`mapAttrs`/`zipAttrsWith`'s `f name value` case) -- with NO bytecode,
 * NO Env, NO Thunk involved; see value_util.c's `ll_resolve_lazyapp`.
 *
 * Memoization: forcing the SAME LazyApp object twice must not re-run
 * `fn` a second time. `ll_resolve_lazyapp` provides that by mutating the
 * object in place on its first (successful) resolution: `marker` flips
 * from `LL_LAZYAPP_MARKER` to `LL_LAZYAPP_FORCED_MARKER`, and the
 * computed result overwrites the `fn` slot (reusing its offset, since
 * `fn`/`arg1`/`arg2` are all dead the instant `marker` says RESOLVED).
 *
 * Deliberately NOT given Thunk's full IN_PROGRESS/FAILED state
 * machinery (heap.h's own Thunk comment, ir.h's LL_THUNK_STATE_*). Two
 * consequences, both accepted and documented rather than silently
 * assumed:
 *   1. No bespoke blackhole detection: a genuine self-referential
 *      LazyApp force cycle recurses through `ll_apply` instead of being
 *      caught by a dedicated IN_PROGRESS check -- it still aborts
 *      (apply.S's own LL_CHECK_RETSP/RETSTACK-overflow guard fires
 *      first), just with a different message than Thunk's own
 *      "blackhole" wording.
 *   2. No memoized-failure state: if resolving a LazyApp throws and
 *      that's caught by an enclosing `builtins.tryEval`, a LATER force
 *      of the SAME object re-runs `fn` from scratch rather than
 *      re-raising the memoized failure the way a Thunk would.
 *
 * Use ll_make_lazyapp1()/ll_make_lazyapp2()/
 * ll_value_is_lazyapp()/ll_resolve_lazyapp() (src/c/value_util.c)
 * -- never poke these offsets directly. Every force chokepoint that
 * already special-cases Cell (force.S, op_var_ref.S, op_select.S,
 * op_select_or.S, op_with.S's OP_WITH_TRY) has an identical sibling
 * check for both LazyApp markers, calling ll_resolve_lazyapp and
 * looping back exactly the way the existing Cell check does. */
#define LL_LAZYAPP_MARKER        (-4)
#define LL_LAZYAPP_FORCED_MARKER (-5)
#define LL_LAZYAPP_OFF_MARKER    0
#define LL_LAZYAPP_OFF_FN        8  /* UNRESOLVED: Value fn; RESOLVED: cached result (reused slot) */
#define LL_LAZYAPP_OFF_RESULT    8  /* alias of OFF_FN -- see above */
#define LL_LAZYAPP_OFF_ARG1      16
#define LL_LAZYAPP_OFF_ARG2      24 /* raw == 0 means "no second argument" (map's 1-arg case) */
#define LL_LAZYAPP_SIZE          32

/* ThunkMap: a SIXTH `LL_TAG_PTR` kind, replacing LazyApp specifically for
 * `mapAttrs`/`zipAttrsWith` (plain `map` over a list keeps using LazyApp
 * -- see below for why this doesn't extend there). Where LazyApp pays
 * one 32-byte heap object PER OUTPUT ELEMENT, ThunkMap pays one ~32-byte
 * ThunkMapDesc heap object PER CALL (shared by every element that call
 * produces) plus, in the best case, ZERO additional bytes per element --
 * the per-element "thunk" IS the Value already sitting in the output
 * attrset's own entry slot, not a separate allocation at all: the
 * entry's own 8-byte Value slot is packed as
 *     (desc_ptr) | (idx << 48) | LL_TAG_PTR
 * rather than pointing at a separate object. This is sound ONLY because
 * real x86-64 Linux user-space pointers are canonical 48-bit addresses --
 * bits 48-63 of any pointer this evaluator ever hands out are ALWAYS zero
 * (TASK_SIZE for x86-64 Linux is 0x7ffffffff000, well under 2^47; verified
 * directly against ll_arena_alloc's own mmap allocations, not just
 * assumed from general x86-64 lore) -- so those bits are free, with NO
 * conflict with the existing low-3-bits tag scheme (value.h's 8 tags are
 * fully saturated, which is exactly why Path/Float/Cell/LazyApp above all
 * had to reuse LL_TAG_PTR plus an in-object marker rather than getting
 * their own tag). This is NOT the same idea as stealing LOW bits above an
 * aligned pointer (unsound -- ordinary bump pointers have no guaranteed-
 * free LOW bits beyond the 3 already spent on the tag); this steals HIGH
 * bits that are free for an entirely different, verified reason (x86-64
 * canonical addressing), the standard "high-bit pointer tagging"
 * technique V8/LuaJIT also use.
 *
 *   ThunkMapDesc:
 *     { int64_t marker == LL_THUNKMAP_MARKER (-6);
 *       Value   f;            // callback: `f name value`
 *       void   *src_entries;  // raw `Attr*`-shaped array (heap.h's packed
 *                              // {uint32_t sym; Value value;}, stride
 *                              // LL_ATTR_SIZE) -- see below for what
 *                              // this points at for each caller
 *       Value   out; }         // the output Attrs Value this call built
 *                              // (always flat, never a union -- see below)
 *   packed element word (lives in `out`'s OWN entries[idx].value slot):
 *     desc_ptr | (idx << 48) | LL_TAG_PTR
 *
 * `src_entries`/`idx` together give "name" (src_entries[idx]'s sym,
 * resolved to a string lazily, only when this specific element is
 * actually forced) and "value" (src_entries[idx]'s own Value, forwarded
 * to `f` unforced).
 *   - `mapAttrs f attrs`: `src_entries` points DIRECTLY at `attrs`'s own
 *     (already-resolved-to-flat) entries array -- no copy, no allocation
 *     at all beyond ThunkMapDesc itself, since `out`'s entries are built
 *     in the exact same sym order as `attrs`'s, so `out`'s index `idx`
 *     and `attrs`'s index `idx` always name the same key.
 *   - `zipAttrsWith f listOfAttrsets`: there is no existing object shaped
 *     like `out` to reuse as `src_entries` -- each key's `value` is a
 *     freshly-collected list of every input attrset's value under that
 *     key. zipAttrsWith2_impl (builtins_m54.c) builds one small
 *     `src_entries` buffer per call (same packed `{sym;value}` shape,
 *     `distinct_n * LL_ATTR_SIZE` bytes) holding each key's
 *     already-collected value list -- this grouping is eager, only the
 *     call to `f` is deferred.
 *
 * Memoization WITHOUT a separate per-element state cell: self-mutates
 * `out`'s own entries[idx].value slot in place, the same "mutate an
 * arena object once, in place, on first successful resolution"
 * precedent LazyApp and Thunk itself already establish:
 *   1. Unforced: out's entries[idx].value slot holds EXACTLY the packed
 *      `desc_ptr|(idx<<48)|LL_TAG_PTR` word ll_resolve_thunkmap was
 *      itself just called with (bit-identical).
 *   2. ll_resolve_thunkmap computes `f name value`, then overwrites
 *      THAT SAME slot with the result -- after which EVERY copy of the
 *      original packed word anywhere else in the program resolves to
 *      the cached result on its next force: ll_resolve_thunkmap always
 *      re-derives the SAME slot address from `desc_ptr`/`idx` (never
 *      from `self`).
 *   3. The check "already forced?" is exactly "does the slot still hold
 *      THIS bit-identical packed word" -- provably never a false
 *      positive: the only way a computed RESULT could coincide bit-for-
 *      bit with `desc_ptr|(idx<<48)|LL_TAG_PTR` is if `f` returned a
 *      value whose own backing object lives at EXACTLY `desc_ptr` -- but
 *      that address already holds a live, distinctly-marked
 *      ThunkMapDesc, and the arena never reuses an address for a second
 *      object.
 *
 * Same two accepted, documented LazyApp consequences apply here: no
 * bespoke blackhole detection for a self-referential ThunkMap cycle
 * (caught by apply.S's ordinary RETSP-overflow guard instead) and no
 * memoized-failure state (a later force of a ThunkMap element whose
 * first attempt threw and was caught by an enclosing `tryEval` re-runs
 * `f` from scratch).
 *
 * Deliberately scoped OUT of plain `map` over a list: a list element's
 * slot IS just a bare `Value` with nowhere to also stash which index it
 * is without an extra word. `map` keeps using LazyApp (ll_make_lazyapp1)
 * unchanged.
 *
 * Bounded by `idx` fitting in the packed word's top 16 bits (`idx <=
 * LL_THUNKMAP_MAX_IDX`, 65535): a `mapAttrs`/`zipAttrsWith` call
 * producing MORE than 65536 output entries falls back to the old
 * per-element LazyApp path instead -- never silently truncates an index.
 *
 * Use ll_thunkmap_make_desc()/ll_thunkmap_pack()/
 * ll_thunkmap_set_out()/ll_value_is_thunkmap()/
 * ll_resolve_thunkmap() (src/c/value_util.c) -- never poke these
 * offsets or the packing scheme directly. Every force chokepoint that
 * already special-cases Cell/LazyApp (force.S, op_var_ref.S, op_select.S,
 * op_select_or.S, op_with.S's OP_WITH_TRY) has an identical sibling
 * check for ThunkMap, and main.c's printer (print_peel_wrappers) keys
 * ancestor-cycle-detection by the per-element `out` slot address
 * (ll_thunkmap_slot) -- NOT by the shared descriptor's own address,
 * which is deliberately the SAME for every element of one call and
 * would otherwise make unrelated sibling elements (e.g. printing
 * `mapAttrs (n: v: v) { a = 1; b = 2; }`) falsely detect each other as
 * "«repeated»". */
#define LL_THUNKMAP_MARKER         (-6)
#define LL_THUNKMAP_OFF_MARKER     0
#define LL_THUNKMAP_OFF_F          8
#define LL_THUNKMAP_OFF_SRC_ENTRIES 16
#define LL_THUNKMAP_OFF_OUT        24
#define LL_THUNKMAP_SIZE           32
#define LL_THUNKMAP_MAX_IDX        0xFFFF /* idx must fit in the packed word's top 16 bits */

/* FormalsMap (`ll`-only, no asnix equivalent): same
 * high-bit-pointer-tagging trick as ThunkMap just above, applied to a
 * DIFFERENT per-call-shared computation -- binding a pattern-matched
 * lambda's REQUIRED formals (`{a, b, ...}:`, no default value) without
 * allocating a separate Thunk per formal. A required formal's binding is
 * always exactly "select this already-known-present field from the call's
 * own attrset argument, forced" -- a fixed, native sequence, not a
 * re-entry into the bytecode interpreter.
 *
 *   FormalsMapDesc:
 *     { int64_t marker == LL_FORMALSMAP_MARKER (-7);
 *       Value  arg;   // the call's own attrset argument (OP_CHECK_FORMALS
 *                      // already validated every required field present)
 *       const int64_t *syms;  // compile-time-shared, one entry per formal
 *                              // POSITION (same order as the fieldframe's
 *                              // own Env slots) -- only ever read at a
 *                              // position this mechanism actually covers
 *       Env   *env; }   // the callee's own fieldframe -- memoization
 *                       // writes back into env->values[idx] directly,
 *                       // there is no separate "out" object the way
 *                       // ThunkMap's per-element Attrs entry is
 *   packed element word (lives in env->values[idx], written by the
 *   OP_FRAME_SET sequence the same way a Thunk pointer would be):
 *     desc_ptr | (idx << 48) | LL_TAG_PTR
 *
 * Memoization mirrors ThunkMap's own "self-mutate the slot this packed
 * word came from" trick exactly (heap.h's LL_THUNKMAP_MARKER comment,
 * point 1-3), just targeting env->values[idx] instead of an Attrs entry.
 */
#define LL_FORMALSMAP_MARKER    (-7)
#define LL_FORMALSMAP_OFF_MARKER 0
#define LL_FORMALSMAP_OFF_ARG    8
#define LL_FORMALSMAP_OFF_SYMS   16
#define LL_FORMALSMAP_OFF_ENV    24
#define LL_FORMALSMAP_SIZE       32
#define LL_FORMALSMAP_MAX_IDX    0xFFFF

/* List: {int64_t count; Value elems[count];} */
#define LL_LIST_OFF_COUNT 0
#define LL_LIST_OFF_ELEMS 8

/* Attrs: {int64_t count; Attr entries[count];}, entries sorted by sym --
 * binary-searched by OP_SELECT. Each Attr is {uint32_t sym; Value value;}
 * PACKED to 12 bytes, no padding.
 *
 * sym is read/written as a plain 4-byte field (`movl` in asm, `uint32_t
 * *` in C) -- NOT as an 8-byte field: entries[i].sym at offset 0 is
 * immediately followed by entries[i].value's own bytes at offset 4, so
 * any 8-byte (`movq`) access to the sym slot would silently
 * read/corrupt the low 4 bytes of this SAME entry's value -- every sym
 * access in this codebase MUST use the 4-byte-wide form. `value` is
 * still accessed as a full 8-byte `Value` (`movq`/`Value *`), at
 * relative offset 4.
 *
 * Entry stride is 12, not a power of 2, so every "index * LL_ATTR_SIZE"
 * computation in hand-written asm uses `imulq $12` rather than a shift.
 *
 * Alignment: `value`'s offset within the overall arena-allocated Attrs
 * object alternates between 8-aligned and 4-aligned (not 8-aligned) as i
 * increases, since 12 isn't a multiple of 8. This is architecturally safe
 * on x86-64: plain `movq`/`Value *` loads and stores never fault on
 * misalignment (only explicitly-aligned SIMD forms like `movdqa` do, and
 * none of those are used on Attr entries), at most a small, rare
 * cache-line-split cost. */
#define LL_ATTRS_OFF_COUNT   0
#define LL_ATTRS_OFF_ENTRIES 8
#define LL_ATTR_OFF_SYM      0
#define LL_ATTR_OFF_VALUE    4
#define LL_ATTR_SIZE         12

/* Lazy union-layering for `//`. `count` (offset 0 above) is read as a
 * signed int64_t and is never negative for an ordinary flat Attrs -- that
 * sign bit is reused as a sentinel: a NEGATIVE value there means this
 * LL_TAG_ATTRS object is actually a UnionHdr, not a flat entries array:
 *
 *   UnionHdr{ int64_t  neg_leaf_count;  // == -(number of leaves), 1..MAX_LEAVES
 *             Value    flat_cache;      // raw==0 (never a legal Value -- see
 *                                        // value.h: raw 0 decodes as a NULL
 *                                        // LL_TAG_PTR, which no live object
 *                                        // ever is) until materialized, then
 *                                        // the fully flattened+deduped,
 *                                        // sym-sorted flat Attrs Value
 *             Value    leaves[-neg_leaf_count]; }  // lowest-priority (oldest)
 *                                                   // leaf first; each leaf is
 *                                                   // itself an ordinary FLAT
 *                                                   // Attrs Value, never
 *                                                   // another UnionHdr --
 *                                                   // ll_attrs_update
 *                                                   // (attrs_update.c) always
 *                                                   // resolves a union RHS
 *                                                   // flat before using it as
 *                                                   // one leaf, so unions
 *                                                   // never nest.
 *
 * `ll_bshared_attrs_resolve` (builtins_shared.c) is the ONE place that
 * knows how to turn a UnionHdr into its flat_cache (materializing +
 * memoizing in place on first need, same "mutate an arena object once,
 * in place" precedent op_select.S's thunk-forcing already established for
 * LL_TAG_THUNK) -- every raw-layout reader of an ATTRS object (every
 * op_*.S binary search, builtins_shared.c's own count/sym/value/find,
 * derivation.c, main.c's printer, builtins_attrs.c, builtins_m54.c) MUST
 * call it first and use ITS result, never poke `count`/`entries` directly
 * on a Value that might be a UnionHdr. */
#define LL_ATTRS_UNION_OFF_NLEAVES   0  /* same slot as OFF_COUNT; stores -(leaf count) */
#define LL_ATTRS_UNION_OFF_FLATCACHE 8
#define LL_ATTRS_UNION_OFF_LEAVES    16
#define LL_ATTRS_UNION_MAX_LEAVES    64

/* PrimOp: {void *fn; int64_t arity;} -- a native builtin function.
 * `fn` has signature `Value (*)(const Value *args, int64_t argc)`; OP_CALL
 * invokes it directly with a pointer straight into VALSTACK (the same
 * contiguous arg layout the Closure call path already reads from) rather
 * than copying into a new Env frame -- primops have no lexical body to
 * capture a scope for. */
#define LL_PRIMOP_OFF_FN      0
#define LL_PRIMOP_OFF_ARITY   8
#define LL_PRIMOP_OFF_NOFORCE 16 /* nonzero means op_call.S's primop-call path must NOT force
                                      * this primop's arguments before calling it (every existing
                                      * primop leaves this 0) -- the one exception is `tryEval`,
                                      * which needs its raw, unforced argument so it can force it
                                      * itself inside its own setjmp/longjmp catch scope (see
                                      * builtins_tryeval.c); forcing it in op_call.S first, before
                                      * tryEval's own catch is even installed, would make tryEval
                                      * unable to catch anything at all. */
#define LL_PRIMOP_SIZE        24

#endif /* LL_HEAP_H */
