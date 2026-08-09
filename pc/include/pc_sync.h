#ifndef PC_SYNC_H
#define PC_SYNC_H

#ifdef __cplusplus
extern "C" {
#endif

// Save sync across this user's own devices, against the prior port's sync-server
// (the sync server). See docs/SAVE_SYNC.md.
//
// The whole feature is off unless BOTH settings.ini keys are set:
//
//   [sync]
//   server = http://mac-mini.local:8787
//   key    = whatever-phrase-you-like
//
// The key is the credential and the pairing mechanism at once: identical key
// on Mac, iPad and iPhone means one shared save. It never goes on the wire —
// the server sees only domain-separated SHA-256 digests of it, and stores
// only a hash of the one it authenticates against.
//
// This is deliberately NOT the prior port's model, where identity is a generated
// seed in a key file that you transcribe between devices. Here the user is
// already editing settings.ini on each device to set the server URL, and on
// iOS that file is reachable in the Files app — so putting the key in the
// same file makes pairing "copy this file to the other two devices" and
// deletes the entire generate/encode/restore subsystem.
//
// What is synced is the whole of memory card A, packed into one blob (both
// the .bin and its .stat sidecar — the sidecar carries the slot number, and
// syncing only the .bin lets fileNo drift between devices).

// Both settings present, so the feature is live. Cheap; safe to call anytime.
int pc_sync_enabled(void);

// Boot: enroll, take the lease, reconcile local card A against the server.
// BLOCKING and deliberately early — this must finish before the game can
// mount the card, or the game would load the old save and then have the file
// replaced underneath it. Short timeouts: an unreachable server costs a few
// seconds and drops to offline, it does not hang the boot.
void pc_sync_startup(void);

// Final upload if anything changed, then release the lease. Registered
// atexit by pc_sync_startup on desktop. On iOS atexit does NOT run — a
// suspended app is killed, not exited — which is what pc_sync_suspend is for.
void pc_sync_shutdown(void);

// Card A was mutated. Cheap and safe to call from the CARD entry points; it
// only sets a flag and a timestamp, the worker thread does the upload.
void pc_sync_notify_card_changed(void);

// iOS lifecycle. suspend: upload now if needed and release the lease, so the
// other devices are not locked out while this one sits in the background.
// resume: retake the lease and reconcile, because anything could have
// happened to the save while we were away. Both BLOCK; suspend must be
// called from inside a UIApplication background task (see pc_window.mm).
void pc_sync_suspend(void);
void pc_sync_resume(void);

// One line for logging: "synced", "offline", "in use by <device>", ...
const char* pc_sync_status(void);

#ifdef __cplusplus
}
#endif

#endif // PC_SYNC_H
