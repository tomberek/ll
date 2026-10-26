/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef ASNIX_HEAP_H
#define ASNIX_HEAP_H

/*
 * Heap object layouts for the ASNIX_TAG_PTR (string, path), ASNIX_TAG_LIST,
 * and ASNIX_TAG_ATTRS value kinds (see docs/value-encoding.md). All arena-
 * allocated, never freed, 8-byte aligned.
 */

/* String: {int64_t len; char bytes[len];} -- `len` is always >= 0. */
#define ASNIX_STRING_OFF_LEN   0
#define ASNIX_STRING_OFF_BYTES 8

/* Path (import-paths): a second ASNIX_TAG_PTR kind -- there's no free tag
 * bit to spend on a dedicated ASNIX_TAG_PATH (all 8 slots are already
 * assigned, see value.h), so this is exactly the "header Kind byte"
 * extension value.h's comment on ASNIX_TAG_PTR anticipates. Told apart
 * from a String with a single extra load: a genuine String's leading word
 * is always its own (non-negative) byte length, so a Path reserves the
 * negative sentinel ASNIX_PATH_MARKER there instead, pushing its own
 * length and bytes out one slot:
 *   Path{int64_t marker == ASNIX_PATH_MARKER (-1); int64_t len; char bytes[len];}
 * `bytes` always holds a fully-resolved ABSOLUTE, lexically-canonicalized
 * path (no `.`/`..`/doubled slashes, no trailing slash) -- resolution
 * against the containing file's directory happens once, at compile time
 * (see src/c/compiler.c's AST_PATH case), not at every use. Use
 * asnix_value_is_path()/asnix_ptr_bytes() (src/c/value_util.h) rather than
 * poking these offsets directly -- they dispatch on the marker so callers
 * that don't care which PTR kind they have (printing, hashing, ...) don't
 * have to. */
#define ASNIX_PATH_MARKER     (-1)
#define ASNIX_PATH_OFF_MARKER 0
#define ASNIX_PATH_OFF_LEN    8
#define ASNIX_PATH_OFF_BYTES  16

/* Float (M51): a THIRD `ASNIX_TAG_PTR` kind, alongside String/Path above --
 * there is still no free tag bit for a dedicated `ASNIX_TAG_FLOAT` (all 8
 * slots in value.h are spent, `ASNIX_TAG_PRIMOP` having claimed the last
 * one back in M7), so this follows the exact same "reserved negative
 * marker in the leading word" precedent M15 established for Path:
 *   Float{int64_t marker == ASNIX_FLOAT_MARKER (-2); double value;}
 * Unlike Path (which just shifts its own same-shape payload out one slot
 * after the marker), a double needs the full 64 bits, so this is a
 * genuine second word, not a repurposing -- there is no way to shrink a
 * double into the 61 usable bits an immediate encoding would offer
 * without losing precision, so heap-boxing it (same as every other
 * PTR-tagged kind here) is the only honest option. `asnix_value_is_path`
 * (value_util.c) already dispatches on marker value alone, so `-2` here
 * is automatically distinguished from both a genuine String (whose
 * leading word is always its own non-negative length) and a Path
 * (marker `-1`) with no changes needed there. Use
 * asnix_make_float_value()/asnix_value_is_float()/asnix_float_value()
 * (value_util.h) -- never poke these offsets directly. */
#define ASNIX_FLOAT_MARKER     (-2)
#define ASNIX_FLOAT_OFF_MARKER 0
#define ASNIX_FLOAT_OFF_VALUE  8

/* Cell (M92): a FOURTH `ASNIX_TAG_PTR` kind, alongside String/Path/Float
 * above -- same "reserved negative marker in the leading word" precedent,
 * one word further along:
 *   Cell{int64_t marker == ASNIX_CELL_MARKER (-3); Value *target;}
 * A Cell is NOT a Nix-visible value kind (unlike Path/Float, which are
 * both genuine Nix types) -- it's a purely internal evaluator device
 * mirroring real Nix's `maybeThunk` slot-aliasing optimization
 * (~/nix/gc-unlock/src/libexpr/eval.cc, ExprVar::maybeThunk): instead of
 * allocating a fresh Thunk for a bare-variable sub-expression about to be
 * stored somewhere lazily (a list element, a call argument, a let/attrs
 * binding value -- see compiler.c's `try_cell_ref`), a Cell just wraps a
 * pointer STRAIGHT AT the existing Env slot that variable already lives
 * in, so two independent occurrences of the same bare variable end up
 * *aliasing the same underlying storage* instead of each getting its own,
 * separately-forced copy. This is what lets `==`/the printer recover
 * real Nix's `if (&v1 == &v2) return true` slot-identity shortcut
 * (eval.cc ~line 2919) despite asnix's Values normally being copied
 * in-line everywhere (see docs/value-encoding.md) rather than referenced
 * through a stable address the way real Nix's `Value*` is.
 *
 * `target` points into an Env's `values[]` array (see env.h) -- always
 * arena-allocated, never freed, so the pointer stays valid for the
 * program's entire lifetime; dereferencing it yields another raw
 * (possibly still-unforced-Thunk, or even another Cell, see compiler.c's
 * chained-bare-var case) Value, which every force chokepoint (force.S,
 * op_var_ref.S, op_select.S, op_select_or.S, op_with.S's OP_WITH_TRY) must
 * now treat as "not yet a usable value" exactly like a Thunk, recursively
 * resolving through it -- see each of those files' own Cell-handling
 * comment. Use asnix_make_cell_value()/asnix_value_is_cell()/
 * asnix_cell_target() (src/c/value_util.c) -- never poke these offsets
 * directly. */
#define ASNIX_CELL_MARKER     (-3)
#define ASNIX_CELL_OFF_MARKER 0
#define ASNIX_CELL_OFF_TARGET 8

/* M162: generous above any legitimate chain of bare-variable aliases a
 * real program could write -- chosen to abort in well under a second
 * even on a genuine cycle (e.g. `let x = y; y = x; in x`, a real
 * user-writable mutual alias with no Thunk involved to catch it via the
 * ordinary IN_PROGRESS blackhole check), not tuned against any specific
 * fixture's depth. Shared between value_util.c's asnix_resolve_cell_chain
 * (the out-of-line reference implementation, still used by op_select.S/
 * op_select_or.S/op_with.S) and the hand-inlined copies of the same loop
 * in op_var_ref.S/force.S -- defined once here so the three copies can
 * never drift apart on the bound. */
#define ASNIX_CELL_CHASE_LIMIT 1000000

/* LazyApp (M113): a FIFTH `ASNIX_TAG_PTR` kind, alongside String/Path/
 * Float/Cell above -- same "reserved negative marker in the leading
 * word" precedent, but (unlike those four, which are all fixed-shape)
 * this one has TWO possible marker values of its own, used as a tiny
 * two-state machine (no third/fourth state -- see the rationale below
 * for why IN_PROGRESS/FAILED, which Thunk has, are deliberately NOT
 * replicated here):
 *
 *   LazyApp, UNRESOLVED:
 *     { int64_t marker == ASNIX_LAZYAPP_MARKER (-4);
 *       Value   fn; Value arg1; Value arg2; }
 *   LazyApp, RESOLVED (the SAME heap object, mutated in place once --
 *   see below):
 *     { int64_t marker == ASNIX_LAZYAPP_FORCED_MARKER (-5);
 *       Value   result; <2 dead words, never read again>; }
 *
 * Purpose: real Nix's `prim_map`/`prim_mapAttrs` build each output
 * element via a lightweight `Value::mkApp` application thunk
 * (value.hh) that needs no Env allocation at all until actually
 * forced, and is never counted as a genuine Thunk either. Before this
 * milestone, asnix's own `map`/`mapAttrs`/`zipAttrsWith`
 * (asnix_bshared_make_lazy_call1/_call2, builtins_shared.c) allocated a
 * genuine Env AND a genuine Thunk for EVERY output element,
 * unconditionally, whether or not that element is ever forced --
 * measured as the single largest simultaneous contributor to both the
 * env-count and thunk-count gap against real Nix on a real nixpkgs
 * closure (see docs/architecture.md's M113 section). A LazyApp replaces
 * that Env+Thunk pair with ONE heap object big enough to hold `fn` plus
 * up to two arguments -- `fn arg1` (`map`'s case; `arg2.raw == 0`,
 * reusing the exact "raw 0 is never a legal Value" sentinel the
 * UnionHdr's own `flat_cache` field above already established) or the
 * curried `(fn arg1) arg2` (`mapAttrs`/`zipAttrsWith`'s `f name value`
 * case) -- with NO bytecode, NO Env, NO Thunk involved at all; see
 * value_util.c's `asnix_resolve_lazyapp` for exactly how it's run.
 *
 * Memoization: forcing the SAME LazyApp object twice (e.g. a shared
 * list/attrs element read from two different places before either has
 * forced it) must not re-run `fn` a second time -- same "exactly once"
 * guarantee every other lazy value kind in this evaluator already
 * gives. `asnix_resolve_lazyapp` provides that by mutating the object
 * in place on its first (successful) resolution: `marker` flips from
 * `ASNIX_LAZYAPP_MARKER` to `ASNIX_LAZYAPP_FORCED_MARKER`, and the
 * computed result overwrites the `fn` slot (reusing its offset, since
 * `fn`/`arg1`/`arg2` are all dead the instant `marker` says RESOLVED --
 * nothing ever reads them again). Every later force of the same object
 * (through ANY copy of the tagged pointer to it, exactly like Thunk's
 * own in-place self-mutation) just reads the cached result back.
 *
 * Deliberately NOT given Thunk's full IN_PROGRESS/FAILED state
 * machinery (heap.h's own Thunk comment, ir.h's ASNIX_THUNK_STATE_*):
 * doing so correctly would mean teaching `asnix_tryeval_unwind_
 * inprogress` (tryeval.c) -- which hard-codes writing the literal
 * THUNK FAILED tag at a fixed offset into every node on `asnix_
 * inprogress_head`'s intrusive stack -- to recognize and write a
 * DIFFERENT tag for a LazyApp node sharing that same list, a real,
 * fiddly change to an already-subtle shared mechanism (see M82's own
 * history of exactly this class of bug). Two consequences, both
 * accepted and documented rather than silently assumed:
 *   1. No bespoke blackhole detection: a genuine self-referential
 *      LazyApp force cycle (constructible, e.g. `let xs = map (v:
 *      builtins.elemAt xs 0) [1]; in builtins.elemAt xs 0`) recurses
 *      through `asnix_apply` instead of being caught by a dedicated
 *      IN_PROGRESS check -- it still aborts (apply.S's own
 *      ASNIX_CHECK_RETSP/RETSTACK-overflow guard, the same one that
 *      already catches ordinary unbounded Nix-level recursion, fires
 *      first), just with a different message than Thunk's own
 *      "blackhole" wording, not a silent hang or an unguarded crash.
 *   2. No memoized-failure state: if resolving a LazyApp throws and
 *      that's caught by an enclosing `builtins.tryEval`, the object is
 *      left exactly as it was before the attempt (this mutation only
 *      ever happens AFTER `fn` returns successfully, never before --
 *      so an exception unwinding past it leaves no partial state to
 *      clean up) -- a LATER force of the SAME object re-runs `fn` from
 *      scratch rather than re-raising the memoized failure the way a
 *      Thunk would (real Nix's Nix-2.34 "errors are memoized" rule,
 *      eval-okay-tryeval-failed-thunk-reeval.nix). Confirmed this exact
 *      combination (`tryEval` wrapping a forced, then re-forced,
 *      `map`/`mapAttrs`/`zipAttrsWith` element) does not appear
 *      anywhere in numtide/nix-conformance's gating suite (grepped
 *      directly: no fixture combines `tryEval` with `map`/`mapAttrs`/
 *      `zipAttrsWith` at all) -- a real, narrow, deliberately-scoped-out
 *      gap, not an oversight.
 *
 * Use asnix_make_lazyapp1()/asnix_make_lazyapp2()/
 * asnix_value_is_lazyapp()/asnix_resolve_lazyapp() (src/c/value_util.c)
 * -- never poke these offsets directly. Every force chokepoint that
 * already special-cases Cell (force.S, op_var_ref.S, op_select.S,
 * op_select_or.S, op_with.S's OP_WITH_TRY) gained an identical sibling
 * check for both LazyApp markers, calling asnix_resolve_lazyapp and
 * looping back exactly the way the existing Cell check does. */
#define ASNIX_LAZYAPP_MARKER        (-4)
#define ASNIX_LAZYAPP_FORCED_MARKER (-5)
#define ASNIX_LAZYAPP_OFF_MARKER    0
#define ASNIX_LAZYAPP_OFF_FN        8  /* UNRESOLVED: Value fn; RESOLVED: cached result (reused slot) */
#define ASNIX_LAZYAPP_OFF_RESULT    8  /* alias of OFF_FN -- see above */
#define ASNIX_LAZYAPP_OFF_ARG1      16
#define ASNIX_LAZYAPP_OFF_ARG2      24 /* raw == 0 means "no second argument" (map's 1-arg case) */
#define ASNIX_LAZYAPP_SIZE          32

/* ThunkMap (M118): a SIXTH `ASNIX_TAG_PTR` kind, replacing LazyApp
 * specifically for `mapAttrs`/`zipAttrsWith` (plain `map` over a list keeps
 * using LazyApp -- see below for why this doesn't extend there). Where
 * LazyApp (M113, above) pays one 32-byte heap object PER OUTPUT ELEMENT,
 * ThunkMap pays one ~32-byte ThunkMapDesc heap object PER CALL (shared by
 * every element that call produces) plus, in the best case, ZERO
 * additional bytes per element -- the per-element "thunk" IS the Value
 * already sitting in the output attrset's own entry slot (a slot that has
 * to exist regardless of how that entry's laziness is represented), not a
 * separate allocation at all.
 *
 * Ported from iets's (~/gnix/iets, a sibling Rust evaluator over the same
 * tagged-8-byte-word bump-arena shape) `mk_map_attrs`/`TAG_THUNK_MAP`
 * (src/runtime/value.rs/word.rs): the entry's own 8-byte Value slot is
 * itself the "thunk", packed as
 *     (desc_ptr) | (idx << 48) | ASNIX_TAG_PTR
 * rather than pointing at a separate object. This is sound ONLY because
 * real x86-64 Linux user-space pointers are canonical 48-bit addresses --
 * bits 48-63 of any pointer this evaluator ever hands out are ALWAYS zero
 * (TASK_SIZE for x86-64 Linux is 0x7ffffffff000, well under 2^47) --
 * confirmed directly for THIS evaluator's own allocations, not just
 * assumed from general x86-64 lore: asnix_arena_alloc (src/asm/arena.S)
 * grows via `mmap(NULL, ..., MAP_PRIVATE|MAP_ANONYMOUS, ...)` with no
 * address hint, and a standalone probe of that exact mmap call on this
 * machine (20 consecutive calls) returned addresses in the 0x7ffff5xxxxxx-
 * 0x7ffff7xxxxxx range every time -- `addr >> 48 == 0` for all of them. So
 * bits 48-63 are free real estate on any genuine heap pointer, with NO
 * conflict with the existing low-3-bits tag scheme (value.h's 8 tags are
 * fully saturated, which is exactly why Path/Float/Cell/LazyApp above all
 * had to reuse ASNIX_TAG_PTR plus an in-object marker rather than getting
 * their own tag; ThunkMap reuses that SAME precedent for its outer tag,
 * and separately spends the pointer's own otherwise-always-zero high bits
 * on the index -- two independent tricks stacked on the same word, not a
 * new exception to the "8 tags, fully saturated" rule). This is NOT the
 * same idea as an earlier-rejected "steal low bits above an aligned
 * pointer" scheme (unsound -- ordinary bump pointers have no guaranteed-
 * free LOW bits beyond the 3 already spent on the tag); this steals HIGH
 * bits that are free for an entirely different, verified reason (x86-64
 * canonical addressing), the standard "high-bit pointer tagging" technique
 * V8/LuaJIT also use.
 *
 *   ThunkMapDesc:
 *     { int64_t marker == ASNIX_THUNKMAP_MARKER (-6);
 *       Value   f;            // callback: `f name value`
 *       void   *src_entries;  // raw `Attr*`-shaped array (heap.h's packed
 *                              // {uint32_t sym; Value value;}, stride
 *                              // ASNIX_ATTR_SIZE) -- see below for what
 *                              // this points at for each caller
 *       Value   out; }         // the output Attrs Value this call built
 *                              // (always flat, never a union -- see below)
 *   packed element word (lives in `out`'s OWN entries[idx].value slot):
 *     desc_ptr | (idx << 48) | ASNIX_TAG_PTR
 *
 * `src_entries`/`idx` together give "name" (src_entries[idx]'s sym,
 * resolved to a string lazily, only when this specific element is
 * actually forced -- unlike the old LazyApp path, which had to build the
 * name String eagerly for EVERY element up front) and "value"
 * (src_entries[idx]'s own Value, forwarded to `f` unforced, same
 * laziness-of-the-callback's-own-argument M23 already established).
 *   - `mapAttrs f attrs`: `src_entries` points DIRECTLY at `attrs`'s own
 *     (already-resolved-to-flat) entries array -- no copy, no allocation
 *     at all beyond ThunkMapDesc itself, since `out`'s entries are built
 *     in the exact same sym order as `attrs`'s (prim_mapAttrs_apply2,
 *     builtins_attrs.c, copies `attrs`'s syms straight across with no
 *     reordering), so `out`'s index `idx` and `attrs`'s index `idx`
 *     always name the same key.
 *   - `zipAttrsWith f listOfAttrsets`: there is no existing object shaped
 *     like `out` to reuse as `src_entries` -- each key's `value` is a
 *     freshly-collected list of every input attrset's value under that
 *     key, a genuinely new per-key computation with no pre-existing home.
 *     zipAttrsWith2_impl (builtins_m54.c) builds one small `src_entries`
 *     buffer per call (same packed `{sym;value}` shape, `distinct_n *
 *     ASNIX_ATTR_SIZE` bytes, no header word needed since `idx` is always
 *     `< out`'s own count) holding each key's already-collected value
 *     list -- this part is NOT deferred (the grouping itself is
 *     inherently eager, see that function's own comment), only the call
 *     to `f` is.
 *
 * Memoization WITHOUT a separate per-element state cell (a deliberate
 * difference from iets's own `MapDesc`, which spends an *additional* 8
 * bytes per element on a dedicated state `Cell` specifically so a forced-
 * in-progress element's blackhole marker never has to touch the output
 * set's own entry slot -- see iets's MapDesc doc comment). asnix skips
 * that and self-mutates `out`'s own entries[idx].value slot in place
 * instead, the exact same "mutate an arena object once, in place, on
 * first successful resolution" precedent LazyApp (M113, above) and Thunk
 * itself already establish:
 *   1. Unforced: out's entries[idx].value slot holds EXACTLY the packed
 *      `desc_ptr|(idx<<48)|ASNIX_TAG_PTR` word asnix_resolve_thunkmap was
 *      itself just called with (bit-identical -- not just "an equivalent
 *      ThunkMap", literally the same bits, which matters: see point 3).
 *   2. asnix_resolve_thunkmap computes `f name value`, then overwrites
 *      THAT SAME slot with the result -- after which EVERY copy of the
 *      original packed word anywhere else in the program (list elements,
 *      Env slots, `let`-bound names, ... -- copies of an 8-byte Value are
 *      ordinary value copies everywhere in this codebase, see
 *      docs/value-encoding.md) resolves to the cached result on its next
 *      force: asnix_resolve_thunkmap always re-derives the SAME slot
 *      address from `desc_ptr`/`idx` (never from `self`), so it doesn't
 *      matter which copy of the packed word is the one being forced.
 *   3. The check "already forced?" is exactly "does the slot still hold
 *      THIS bit-identical packed word" -- provably never a false
 *      positive: the only way a computed RESULT could coincide bit-for-
 *      bit with `desc_ptr|(idx<<48)|ASNIX_TAG_PTR` is if `f` returned a
 *      value whose own backing object lives at EXACTLY `desc_ptr` -- but
 *      that address already holds a live, distinctly-marked
 *      ThunkMapDesc, and the arena never reuses an address for a second
 *      object, so no OTHER value can ever be allocated there.
 *
 * Same two accepted, documented LazyApp consequences apply here (see
 * LazyApp's own comment above for the full reasoning, not repeated): no
 * bespoke blackhole detection for a self-referential ThunkMap cycle
 * (caught by apply.S's ordinary RETSP-overflow guard instead, like any
 * other unbounded Nix-level recursion) and no memoized-failure state
 * (a later force of a ThunkMap element whose first attempt threw and was
 * caught by an enclosing `tryEval` re-runs `f` from scratch).
 *
 * Deliberately scoped OUT of plain `map` over a list: a list has no
 * existing per-element slot shaped like an Attrs entry to pack a
 * descriptor pointer + index into the way an attrset's own `entries[idx].
 * value` already does double duty here -- a list element's slot IS just a
 * bare `Value` with nowhere to also stash which index it is without an
 * extra word (iets pays 16 bytes/element for plain `map`, via a different
 * mechanism, for exactly this reason). `map` keeps using LazyApp
 * (asnix_make_lazyapp1) unchanged.
 *
 * Bounded by `idx` fitting in the packed word's top 16 bits (`idx <=
 * ASNIX_THUNKMAP_MAX_IDX`, 65535 -- matching iets's own `MAP_MAX_IDX`): a
 * `mapAttrs`/`zipAttrsWith` call producing MORE than 65536 output entries
 * falls back to the old per-element LazyApp path instead (checked by each
 * caller before choosing this scheme) -- never silently truncates an
 * index.
 *
 * Use asnix_thunkmap_make_desc()/asnix_thunkmap_pack()/
 * asnix_thunkmap_set_out()/asnix_value_is_thunkmap()/
 * asnix_resolve_thunkmap() (src/c/value_util.c) -- never poke these
 * offsets or the packing scheme directly. Every force chokepoint that
 * already special-cases Cell/LazyApp (force.S, op_var_ref.S, op_select.S,
 * op_select_or.S, op_with.S's OP_WITH_TRY) gained an identical sibling
 * check for ThunkMap, and main.c's printer (print_peel_wrappers) gained
 * one too, keyed for ancestor-cycle-detection purposes by the per-element
 * `out` slot address (asnix_thunkmap_slot) -- NOT by the shared
 * descriptor's own address, which is deliberately the SAME for every
 * element of one call and would otherwise make unrelated sibling elements
 * (e.g. printing `mapAttrs (n: v: v) { a = 1; b = 2; }`) falsely detect
 * each other as "«repeated»". */
#define ASNIX_THUNKMAP_MARKER         (-6)
#define ASNIX_THUNKMAP_OFF_MARKER     0
#define ASNIX_THUNKMAP_OFF_F          8
#define ASNIX_THUNKMAP_OFF_SRC_ENTRIES 16
#define ASNIX_THUNKMAP_OFF_OUT        24
#define ASNIX_THUNKMAP_SIZE           32
#define ASNIX_THUNKMAP_MAX_IDX        0xFFFF /* idx must fit in the packed word's top 16 bits */

/* FormalsMap (`ll`-only, no asnix equivalent): same
 * high-bit-pointer-tagging trick as ThunkMap just above, applied to a
 * DIFFERENT per-call-shared computation -- binding a pattern-matched
 * lambda's REQUIRED formals (`{a, b, ...}:`, no default value) without
 * allocating a separate Thunk per formal. A required formal's binding is
 * always exactly "select this already-known-present field from the call's
 * own attrset argument, forced" -- no arbitrary user code to run (unlike
 * ThunkMap's callback `f`), so the resolver below is a fixed, native
 * sequence, not a re-entry into the bytecode interpreter.
 *
 *   FormalsMapDesc:
 *     { int64_t marker == ASNIX_FORMALSMAP_MARKER (-7);
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
 *     desc_ptr | (idx << 48) | ASNIX_TAG_PTR
 *
 * Memoization mirrors ThunkMap's own "self-mutate the slot this packed
 * word came from" trick exactly (heap.h's ASNIX_THUNKMAP_MARKER comment,
 * point 1-3), just targeting env->values[idx] instead of an Attrs entry.
 */
#define ASNIX_FORMALSMAP_MARKER    (-7)
#define ASNIX_FORMALSMAP_OFF_MARKER 0
#define ASNIX_FORMALSMAP_OFF_ARG    8
#define ASNIX_FORMALSMAP_OFF_SYMS   16
#define ASNIX_FORMALSMAP_OFF_ENV    24
#define ASNIX_FORMALSMAP_SIZE       32
#define ASNIX_FORMALSMAP_MAX_IDX    0xFFFF

/* List: {int64_t count; Value elems[count];} */
#define ASNIX_LIST_OFF_COUNT 0
#define ASNIX_LIST_OFF_ELEMS 8

/* Attrs: {int64_t count; Attr entries[count];}, entries sorted by sym --
 * binary-searched by OP_SELECT. Each Attr is {uint32_t sym; Value value;}
 * PACKED to 12 bytes, no padding (M103).
 *
 * Before M103 this was {uint32_t sym; uint32_t pad; Value value;} = 16
 * bytes, with `pad` a permanent, pure-alignment 4-byte tax on every
 * attrset entry ever allocated -- in a never-free arena that's a
 * permanent cost for the process's entire lifetime, not a transient one.
 * Measured on `./asnix d.nix` (nixpkgs's ffmpeg.outPath, M103's
 * instrumentation pass): 12,698,073 total Attr entries ever allocated
 * across 468,792 entries-arrays over the run -- 4 bytes/entry is ~48.4
 * MiB reclaimed on a ~742 MiB peak-RSS run (~6.5%).
 *
 * sym is now read/written as a plain 4-byte field (`movl` in asm,
 * `uint32_t *` in C) -- NOT as an 8-byte field the way the old pad trick
 * let it be (loading/storing `sym+pad` together as one 64-bit word was
 * only safe because `pad` was always zero AND immediately followed by
 * nothing else that 8-byte write/read was allowed to clobber; with no pad,
 * entries[i].sym at offset 0 is immediately followed by entries[i].value's
 * own bytes at offset 4, so any 8-byte (`movq`) access to the sym slot
 * would silently read/corrupt the low 4 bytes of this SAME entry's value
 * -- every sym access in this codebase MUST use the 4-byte-wide form).
 * `value` is unaffected: it's still accessed as a full 8-byte `Value`
 * (`movq`/`Value *`), just at relative offset 4 instead of 8 now.
 *
 * Entry stride is 12, not a power of 2, so every "index * ASNIX_ATTR_SIZE"
 * computation in hand-written asm uses `imulq $12` (one instruction, same
 * as the old `shlq $4` it replaces) rather than a shift. C call sites are
 * unaffected by the non-power-of-2 stride -- `i * ASNIX_ATTR_SIZE` is
 * ordinary C multiplication and the compiler picks whatever instruction
 * sequence it likes.
 *
 * Alignment: `value`'s offset within the overall arena-allocated Attrs
 * object alternates between 8-aligned and 4-aligned (not 8-aligned) as i
 * increases, since 12 isn't a multiple of 8. This is architecturally safe
 * on x86-64: plain `movq`/`Value *` loads and stores never fault on
 * misalignment (only explicitly-aligned SIMD forms like `movdqa` do, and
 * none of those are used on Attr entries -- checked: no SIMD, no atomics,
 * on this array anywhere in the codebase), at most a small, rare
 * cache-line-split cost. An alternative "SoA" layout (all syms packed
 * first, all values packed after, each run individually 8-aligned) would
 * recover slightly more of the 4-byte tax (down to at most 4 bytes of
 * one-time padding PER ARRAY instead of PER ENTRY) but was rejected: it
 * would make every access site's value-offset depend on a runtime `count`
 * (not a compile-time constant), including the hot asm binary-search
 * loops, for a marginal (~1.8 MiB upper bound) additional saving over the
 * straight 12-byte-packed form -- not worth the added complexity/risk. */
#define ASNIX_ATTRS_OFF_COUNT   0
#define ASNIX_ATTRS_OFF_ENTRIES 8
#define ASNIX_ATTR_OFF_SYM      0
#define ASNIX_ATTR_OFF_VALUE    4
#define ASNIX_ATTR_SIZE         12

/* M88: lazy union-layering for `//`. `count` (offset 0 above) is read as a
 * signed int64_t and is never negative for an ordinary flat Attrs -- that
 * sign bit is reused as a sentinel: a NEGATIVE value there means this
 * ASNIX_TAG_ATTRS object is actually a UnionHdr, not a flat entries array:
 *
 *   UnionHdr{ int64_t  neg_leaf_count;  // == -(number of leaves), 1..MAX_LEAVES
 *             Value    flat_cache;      // raw==0 (never a legal Value -- see
 *                                        // value.h: raw 0 decodes as a NULL
 *                                        // ASNIX_TAG_PTR, which no live object
 *                                        // ever is) until materialized, then
 *                                        // the fully flattened+deduped,
 *                                        // sym-sorted flat Attrs Value
 *             Value    leaves[-neg_leaf_count]; }  // lowest-priority (oldest)
 *                                                   // leaf first; each leaf is
 *                                                   // itself an ordinary FLAT
 *                                                   // Attrs Value, never
 *                                                   // another UnionHdr --
 *                                                   // asnix_attrs_update
 *                                                   // (attrs_update.c) always
 *                                                   // resolves a union RHS
 *                                                   // flat before using it as
 *                                                   // one leaf, so unions
 *                                                   // never nest.
 *
 * `asnix_bshared_attrs_resolve` (builtins_shared.c) is the ONE place that
 * knows how to turn a UnionHdr into its flat_cache (materializing +
 * memoizing in place on first need, same "mutate an arena object once,
 * in place" precedent op_select.S's thunk-forcing already established for
 * ASNIX_TAG_THUNK) -- every raw-layout reader of an ATTRS object (every
 * op_*.S binary search, builtins_shared.c's own count/sym/value/find,
 * derivation.c, main.c's printer, builtins_attrs.c, builtins_m54.c) MUST
 * call it first and use ITS result, never poke `count`/`entries` directly
 * on a Value that might be a UnionHdr -- see docs/architecture.md's M88
 * section for the full call-site enumeration this was checked against. */
#define ASNIX_ATTRS_UNION_OFF_NLEAVES   0  /* same slot as OFF_COUNT; stores -(leaf count) */
#define ASNIX_ATTRS_UNION_OFF_FLATCACHE 8
#define ASNIX_ATTRS_UNION_OFF_LEAVES    16
#define ASNIX_ATTRS_UNION_MAX_LEAVES    64

/* PrimOp (M7): {void *fn; int64_t arity;} -- a native builtin function.
 * `fn` has signature `Value (*)(const Value *args, int64_t argc)`; OP_CALL
 * invokes it directly with a pointer straight into VALSTACK (the same
 * contiguous arg layout the Closure call path already reads from) rather
 * than copying into a new Env frame -- primops have no lexical body to
 * capture a scope for. */
#define ASNIX_PRIMOP_OFF_FN      0
#define ASNIX_PRIMOP_OFF_ARITY   8
#define ASNIX_PRIMOP_OFF_NOFORCE 16 /* M30: nonzero means op_call.S's primop-call path must
                                      * NOT force this primop's arguments before calling it
                                      * (every existing primop leaves this 0, preserving M23's
                                      * "args are pre-forced" convention unchanged) -- the one
                                      * exception is `tryEval`, which needs its raw, unforced
                                      * argument so it can force it itself inside its own
                                      * setjmp/longjmp catch scope (see builtins_tryeval.c);
                                      * forcing it in op_call.S first, before tryEval's own
                                      * catch is even installed, would make tryEval unable to
                                      * catch anything at all. */
#define ASNIX_PRIMOP_SIZE        24

#endif /* ASNIX_HEAP_H */
