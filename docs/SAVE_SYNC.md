# Save sync

One save file shared across a Mac, an iPad and an iPhone, over
[the prior port's sync server](../../the prior port/sync-server/). Off unless configured.

The problem it solves is not backup — it is that three devices with one save
is a clobbering machine. You play a day on the iPad, forget, and start the
Mac. So the centre of this is a **lease**, not a file copy: exactly one device
may hold the save at a time, and the others are told who has it.

## Setting it up

Run the server somewhere that stays on (`node server.js` in
`the prior port/sync-server`; zero dependencies, any Node ≥ 18). Then in
`settings.ini` on **each** device:

```ini
[sync]
server = http://mac-mini.local:8787
key    = whatever-phrase-you-invent
device = ipad
```

`server` and `key` must be identical everywhere; `device` is just the name the
other devices see. That is the whole pairing step — there is no account, no
login, and no code to transcribe.

On iOS `settings.ini` lives in `Documents/`, which is exposed to the Files app
(`UIFileSharingEnabled`), so the file is editable on the device itself.

**The key is the credential.** It never goes on the wire — the server only
ever sees `sha256("PIKMIN-SYNC-ID-V1" + key)` as an identifier and
`sha256("PIKMIN-SYNC-AUTH-V1" + key)` as a bearer secret, and stores only a
hash of the second — but anyone who has the phrase has the save. On a LAN that
is a fine trade. Across the internet, put the server behind TLS and use an
`https://` URL.

## What happens when

| | |
|---|---|
| **Boot** | enroll → take the lease → reconcile against the server, before the game can mount the card |
| **Playing** | every card mutation marks it dirty; a worker uploads once the card has been quiet for 3s, and heartbeats the lease every 60s |
| **Quitting** | final upload, release the lease |
| **iOS background** | the same as quitting, inside a `UIApplication` background task — `atexit` never runs on iOS, a suspended app is killed |
| **iOS foreground** | retake the lease and reconcile, because another device may have played meanwhile |

Reconcile, in full. The server is the source of truth once a device has
enrolled, but **nothing local is ever destroyed**:

- server has nothing → publish this device's card
- newest snapshot is ours, card unchanged → nothing to do
- newest snapshot is ours, card changed → upload (progress made offline)
- server moved on, card unchanged since our last sync → check it out, keeping
  the previous card as `sync/presync-<ts>.pks`
- server moved on **and** the card changed → conflict. The server's copy wins,
  and this device's card is kept as `sync/conflict-<ts>.pks`. Said loudly,
  because it is the one case you may actually want to reverse.

If the server is unreachable or another device holds the lease, the game plays
offline: no lease, no uploads, and the next boot reconciles. Nothing blocks.

## What gets synced

All of memory card A, packed into one blob — both `<name>.bin` **and** its
`<name>.stat` sidecar. The sidecar carries the directory slot number, and
syncing only the `.bin` would let `fileNo` drift between devices. Rotated
`.bakN` backups stay local.

The pack is trivial and self-describing:

```
"PKMNCARD" u32 version u32 count
count x { u32 nameLen, name, u32 dataLen, data }
```

Names are sorted, so the same card always produces byte-identical bytes — the
hash is what tells "this device saved" from "nothing changed", and an unstable
order would fake a conflict on every boot.

## Recovering something

Snapshots are kept forever by default (`SNAPSHOTS_KEEP=0`), so the server
holds a copy of every save the three devices ever uploaded, and Pikmin only
writes the card at day end — so that is roughly one snapshot per in-game day.

```bash
curl -H "Authorization: Bearer <id>.<auth>" http://your-server:8787/api/snapshots
```

The client has no restore UI. To roll back, fetch a snapshot with that API and
unpack it over `save/card_a/` by hand — the format above is four fields, and a
`.pks` file kept by a conflict is the same format.

## Notes

- **The lease is per device, not per save.** The server lets the *same* device
  retake its own lease after a crash, and refuses any other. So an app that
  dies mid-session does not lock you out of your own save for the TTL.
- **A crash leaves the save flagged `dirty`** until the lease TTL expires
  (5 min default). The next reconcile says so on stdout; the ordinary rules
  still apply, since the crashed device's last upload is still the newest
  snapshot.
- **iOS local networking needs permission.** `NSLocalNetworkUsageDescription`
  and an ATS exception are in `Pikmin/Info.plist`. Without the prompt being
  accepted the server simply reads as unreachable.
- **The friends/visiting half of the server is unused.** It exists for Animal
  Crossing town visits; Pikmin enrolls with a derived friend code purely
  because the endpoint requires the field.
