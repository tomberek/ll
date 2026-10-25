/* ported from asnix@0ad7e2f60019b9cadf8205a6e2c77885d63af445 (src/c, include/asnix, src/asm) -- see ~/.claude/plans/soft-wishing-engelbart.md */
#ifndef ASNIX_DAEMON_CLIENT_H
#define ASNIX_DAEMON_CLIENT_H

#include <stddef.h>

/*
 * A minimal client for the real Nix worker (daemon) protocol, scoped to
 * connect + handshake + `IsValidPath` (the simplest, read-only op, picked
 * as the first one to prove the handshake/framing mechanism against a
 * real daemon before expanding -- same "smallest slice first" discipline
 * as M8's derivation work). See docs/architecture.md's M8 notes for the
 * full writeup: wire format, magic numbers, and op codes were cross-
 * checked against two independent real implementations before writing
 * any code -- ~/nix/src/libstore/include/nix/store/worker-protocol.hh
 * (cppnix's own header) and ~/gnix/fix/src/store/daemon/{wire,client}.zig
 * (a from-scratch reimplementation) agree exactly on both.
 *
 * Deliberately narrow: no build/realization ops, no NAR transfer, no
 * structured-activity forwarding (the stderr sideband's start/stop-
 * activity and result messages are skipped, not acted on) -- just enough
 * to ask a live daemon "is this store path valid" and get a real answer.
 */

typedef struct AsnixDaemonConn AsnixDaemonConn;

/* Connects to the daemon over its Unix socket (NIX_REMOTE if set, else
 * /nix/var/nix/daemon-socket/socket) and completes the version handshake.
 * Returns NULL (after printing a message to stderr) on any failure --
 * socket connect, magic mismatch, or incompatible protocol major version. */
AsnixDaemonConn *asnix_daemon_connect(void);

void asnix_daemon_close(AsnixDaemonConn *conn);

/* Returns 1 if `path` is a valid store path, 0 if not, -1 on a protocol
 * or I/O error (message already printed to stderr). */
int asnix_daemon_is_valid_path(AsnixDaemonConn *conn, const char *path);

/* Adds a .drv's ATerm text to the store via AddToStore (op 7; the older
 * op 8 AddTextToStore is obsolete since protocol 1.25 -- this daemon
 * reports minor 35, so op 7 is the real path real `nix`/Lix clients take
 * too, cross-checked against ~/nix's remote-store.cc and ~/gnix/fix's
 * client.zig before writing this). Content-addressed as "text:sha256"
 * with an empty references list (asnix's derivations have no input
 * derivations/sources yet -- see derivation.h's scope notes), `repair`
 * always sent false (an untrusted client sending `repair=true` is
 * rejected outright). Self-verifying content -- no trust/signing needed
 * for this op itself, unlike the build step below. Returns a malloc'd
 * copy of the daemon's computed store path (caller frees) -- compare
 * against the caller's own locally-computed drv_path as a reproducibility
 * check -- or NULL on a protocol/IO error (message already printed to
 * stderr). */
char *asnix_daemon_add_text_to_store(AsnixDaemonConn *conn, const char *name, const char *text, size_t text_len);

/* Triggers a build of every output of a .drv already in the store, via
 * BuildPaths (op 9; BuildDerivation=36 sends the whole derivation inline
 * instead of referencing an already-added store path, and is strictly
 * worse for an untrusted client -- see this function's .c comment).
 * Returns 0 on success, -1 on any failure, INCLUDING a clean daemon-side
 * rejection (e.g. "you are not privileged to build input-addressed
 * derivations" for a client not in `trusted-users`/`allowed-users` --
 * that is the real, expected daemon permission boundary for an ordinary
 * (non-content-addressed) derivation like the ones derivation.c builds
 * today, not a bug in this client). The rejection reason is surfaced via
 * the normal STDERR_ERROR sideband and printed to stderr like any other
 * daemon error. */
int asnix_daemon_build_drv(AsnixDaemonConn *conn, const char *drv_path);

/* The daemon's own "is this client trusted" verdict from the handshake
 * (protocol >= 1.35 only; -1 if the daemon's protocol predates that field
 * or the connection failed before reaching it). 1 = trusted, 0 = not
 * trusted, 2 = "not set"/unknown per the daemon's own TrustedFlag enum --
 * useful to check before attempting asnix_daemon_build_drv on an ordinary
 * (non-content-addressed) derivation, since an untrusted client will be
 * rejected at that point regardless (see that function's doc comment). */
int asnix_daemon_trusted(AsnixDaemonConn *conn);

/* Sends SetOptions (op 19) -- every real `nix`/Lix client sends this once,
 * right after the handshake, carrying its *own* merged config (CLI flags +
 * nix.conf) to the daemon; without it the daemon just keeps using its own
 * process-wide settings (loaded once from the system /etc/nix/nix.conf at
 * daemon startup) for every connection, substituter list included. This is
 * the actual explanation for a real divergence found while testing this
 * client against the live daemon: a `BuildPaths` call made without ever
 * sending SetOptions failed on an expired auth token for a private Cachix
 * substituter configured in this machine's system nix.conf, while plain
 * `nix-build` on the same expression succeeded -- because nix-build (like
 * any real client) sends SetOptions, and this machine's user-level config
 * takes a different path (a remote SSH builder) that never consults that
 * substituter at all.
 *
 * Deliberately narrow: only `useSubstitutes` is a real, meaningful knob
 * here (0 = skip substituter lookups entirely and go straight to a local
 * build, matching e.g. `--option substitute false`); every other field
 * this op carries (keepFailed/keepGoing/tryFallback/verbosity/
 * maxBuildJobs/maxSilentTime/buildCores, two obsolete int slots, an empty
 * settings-override map) is sent as an inert, sensible default -- exact
 * field order and defaults cross-checked against ~/nix's own
 * RemoteStore::setOptions (remote-store.cc) and its daemon.cc
 * SetOptions handler. Returns 0 on success, -1 on a protocol/IO error. */
int asnix_daemon_set_options(AsnixDaemonConn *conn, int use_substitutes);

#endif /* ASNIX_DAEMON_CLIENT_H */
