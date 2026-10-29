/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef LL_DAEMON_CLIENT_H
#define LL_DAEMON_CLIENT_H

#include <stddef.h>

/*
 * A minimal client for the real Nix worker (daemon) protocol, scoped to
 * connect + handshake + `IsValidPath`. Wire format, magic numbers, and op
 * codes were cross-checked against two independent real implementations
 * -- ~/nix/src/libstore/include/nix/store/worker-protocol.hh (cppnix's
 * own header) and ~/gnix/fix/src/store/daemon/{wire,client}.zig (a
 * from-scratch reimplementation) agree exactly on both.
 *
 * Deliberately narrow: no build/realization ops, no NAR transfer, no
 * structured-activity forwarding (the stderr sideband's start/stop-
 * activity and result messages are skipped, not acted on) -- just enough
 * to ask a live daemon "is this store path valid" and get a real answer.
 */

typedef struct LlDaemonConn LlDaemonConn;

LlDaemonConn *ll_daemon_connect(void);

void ll_daemon_close(LlDaemonConn *conn);

int ll_daemon_is_valid_path(LlDaemonConn *conn, const char *path);

/* Adds a .drv's ATerm text to the store via AddToStore (op 7; the older
 * op 8 AddTextToStore is obsolete since protocol 1.25 -- this daemon
 * reports minor 35, so op 7 is the real path real `nix`/Lix clients take
 * too). Content-addressed as "text:sha256" with an empty references list,
 * `repair` always sent false (an untrusted client sending `repair=true`
 * is rejected outright). Returns a malloc'd copy of the daemon's computed
 * store path (caller frees), or NULL on a protocol/IO error. */
char *ll_daemon_add_text_to_store(LlDaemonConn *conn, const char *name, const char *text, size_t text_len);

/* Triggers a build of every output of a .drv already in the store, via
 * BuildPaths (op 9; BuildDerivation=36 sends the whole derivation inline
 * instead of referencing an already-added store path, and is strictly
 * worse for an untrusted client). Returns 0 on success, -1 on any
 * failure, INCLUDING a clean daemon-side rejection (e.g. "you are not
 * privileged to build input-addressed derivations" for a client not in
 * `trusted-users`/`allowed-users` -- that is the real, expected daemon
 * permission boundary for an ordinary (non-content-addressed)
 * derivation, not a bug in this client). */
int ll_daemon_build_drv(LlDaemonConn *conn, const char *drv_path);

/* The daemon's own "is this client trusted" verdict from the handshake
 * (protocol >= 1.35 only; -1 if the daemon's protocol predates that field
 * or the connection failed before reaching it). 1 = trusted, 0 = not
 * trusted, 2 = "not set"/unknown per the daemon's own TrustedFlag enum. */
int ll_daemon_trusted(LlDaemonConn *conn);

/* Sends SetOptions (op 19) -- every real `nix`/Lix client sends this once,
 * right after the handshake, carrying its *own* merged config (CLI flags +
 * nix.conf) to the daemon; without it the daemon just keeps using its own
 * process-wide settings for every connection, substituter list included.
 * Confirmed necessary directly: a `BuildPaths` call made without ever
 * sending SetOptions failed on an expired auth token for a private Cachix
 * substituter configured in this machine's system nix.conf, while plain
 * `nix-build` on the same expression succeeded, because nix-build sends
 * SetOptions and this machine's user-level config takes a different path
 * that never consults that substituter at all.
 *
 * Deliberately narrow: only `useSubstitutes` is a real, meaningful knob
 * here (0 = skip substituter lookups entirely and go straight to a local
 * build); every other field this op carries is sent as an inert, sensible
 * default -- exact field order and defaults cross-checked against
 * ~/nix's own RemoteStore::setOptions (remote-store.cc) and its daemon.cc
 * SetOptions handler. Returns 0 on success, -1 on a protocol/IO error. */
int ll_daemon_set_options(LlDaemonConn *conn, int use_substitutes);

#endif /* LL_DAEMON_CLIENT_H */
