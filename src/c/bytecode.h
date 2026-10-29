/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_BYTECODE_H
#define LL_BYTECODE_H

#include <stddef.h>
#include <stdint.h>

#include "ll/ir.h"

/* Portable (de)serialization of an already-compiled IrProgram -- the
 * foundation for an on-disk bytecode cache (bytecode_cache.h/.c). An
 * in-memory IrProgram is NOT directly serializable for two reasons (see
 * docs/architecture.md's M92 notes for the full investigation):
 *
 *   1. ib_finish()/ir_link() rewrite every op's `handler` field from a
 *      small integer op-id into a real function-pointer address in THIS
 *      process's own loaded .text -- a different process (or even a
 *      recompiled binary) has that code at a different address.
 *   2. Several ops' pool slots hold raw in-process pointers (child
 *      IrProgram.ops/.pool for OP_LAMBDA/OP_MK_THUNK) or this-run symbol
 *      ids (ll_intern assigns ids by first-seen order per process, not
 *      by content) -- neither survives a fresh process.
 *
 * ll_bytecode_serialize_program walks the op/pool structure (every
 * pool-referencing op's exact layout is enumerated in ir.h's own OP_*
 * comments -- OP_PUSH_CONST, OP_MK_ATTRS, OP_MK_THUNK, OP_LAMBDA,
 * OP_ASSERT_FAIL, OP_CHECK_FORMALS, OP_REC_OVERRIDES_MERGE) and emits a
 * portable byte stream: op-ids (not handler pointers), symbols by NAME
 * (re-interned on load, see ll_intern), and heap-pointer-shaped pool
 * Values (String/Path/Float/Attrs) by CONTENT, recursing into nested
 * child IrPrograms inline.
 *
 * Fails closed: ANY op-id or Value tag this walk doesn't recognize as one
 * of the above (shouldn't happen given the enumeration above is
 * exhaustive against the current op set, but a future op added without
 * updating this file is exactly the failure mode to guard against)
 * aborts serialization and returns -1 -- the caller's only correct
 * response is to skip caching this compile unit, never to write a
 * partial/wrong cache entry. */

/* Serializes `prog`. On success (0), `*out_buf` is a freshly malloc'd
 * buffer of `*out_len` bytes the caller owns (free() it). On failure
 * (-1), `*out_buf`/`*out_len` are untouched. */
int ll_bytecode_serialize(IrProgram *prog, uint8_t **out_buf, size_t *out_len);

/* Deserializes a buffer produced by ll_bytecode_serialize (possibly
 * read back from disk, possibly corrupt/truncated/foreign) into a fresh
 * IrProgram, re-interning every symbol through ll_intern (so ids
 * match THIS process's own symbol table) and arena-allocating every
 * heap Value fresh. Returns NULL on literally any anomaly (bad magic,
 * truncated buffer, unrecognized tag) -- never partially succeeds. The
 * returned IrProgram's ops are already ir_link()-ed (handler pointers
 * resolved against THIS process's own ll_handler_table), ready for
 * ll_eval() exactly like one ib_finish() just produced. */
IrProgram *ll_bytecode_deserialize(const uint8_t *buf, size_t len);

#endif /* LL_BYTECODE_H */
