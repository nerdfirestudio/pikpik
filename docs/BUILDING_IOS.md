# Building and running on iOS / iPadOS

This assumes you have Xcode and an Apple Developer account, and that you have
not necessarily done this particular dance before. It is written as a
step-by-step; the reasoning behind each step is in the notes at the end.

**Target:** an M-series iPad, or an iPhone, running iPadOS/iOS **16.4 or
later**. The 16.4 floor is deliberate — it is where M-series devices gained BC7
texture support, which the HD texture path relies on rather than shipping a CPU
decoder.

**Xcode 16 or newer is required**, and this is a hard floor rather than a
recommendation: the project file is `objectVersion = 77` and uses a
`PBXFileSystemSynchronizedRootGroup` — the folder-backed group format Xcode 16
introduced — so older Xcode cannot open it at all. Verified with Xcode 26.6 on
macOS 26.5 with CMake 4.3.

That format is also why `app/pikpik/` picks up `main.m` and `Assets.xcassets`
without either being listed in the project file: the group is the directory.
Add a source file there and it joins the target automatically.

**Simulator: no.** The build targets `iphoneos` on arm64 and the engine
archives are device slices. Run it on hardware.

---

## Step 1 — Get the sources

```bash
git clone https://github.com/magikarp/pikpik.git && cd pikpik
./setup.sh
```

`setup.sh` fetches the decompiled game sources into `vendor/pikmin` at a pinned
commit and applies this port's patches. It does not ship with them; see
[LEGAL.md](LEGAL.md).

## Step 2 — Build the engine

```bash
./build_ios.sh
```

Two things happen, and the first only happens once:

1. **SDL2.** If `ios/sdl2-ios/lib/libSDL2.a` is absent, the script downloads the
   official SDL2 **2.32.10** release tarball from libsdl.org and builds a static
   arm64 SDL2 for iOS into `ios/sdl2-ios`. Several minutes.
2. **The engine.** Cross-compiles roughly 450 translation units against the iOS
   SDK and archives them:

   ```
   build-ios/lib/libpikpik.a        the entry point
   build-ios/lib/libpikmin_core.a   the decompiled game   (~65 MB)
   build-ios/lib/libpikmin_pc.a     the port layer
   ```

The first full build takes several minutes. Later ones are incremental.

Neither SDL nor its source is redistributed by this repository — the script
fetches it from the upstream release. See [NOTICE](../NOTICE).

## Step 3 — Open the Xcode project

```bash
open app/pikpik.xcodeproj
```

The project is deliberately thin. It owns signing, the `Info.plist`, the asset
catalog, and deployment — nothing else. All the code is in the three archives
above plus a single `app/pikpik/main.m`, whose only job is to hand control to
SDL's UIKit backend.

Xcode will create a scheme named `pikpik` the first time you open the project.
That is expected — schemes are per-developer state and are not committed.

## Step 4 — Signing

This is the part that is yours, and it is the step that most often goes wrong.

Select the **pikpik** target → **Signing & Capabilities**:

| Field | What to do |
|---|---|
| **Automatically manage signing** | Leave checked |
| **Team** | Pick yours. It ships blank on purpose — a hardcoded team id would fail for everyone who is not in it |
| **Bundle Identifier** | **Change it.** It ships as `com.example.pikpik`, which will not register. Use your own reverse-DNS, e.g. `com.yourname.pikpik` |

Then plug in the device, select it in the toolbar's destination menu, and
press Run.

**On a free account.** Signing works, with two irritations: the certificate
expires after **seven days**, after which the app refuses to launch until you
rebuild from Xcode; and you are limited in how many app IDs you can register.
Also, the first install of any app from a new certificate needs the developer
trusted on the device itself — **Settings → General → VPN & Device Management
→ [your Apple ID] → Trust**. The app will not launch before you do that, and
the error message does not make the reason obvious.

**On a paid account.** The certificate lasts a year and the app ID limits go
away. Nothing else differs.

**First run on a modern iOS also needs Developer Mode** on the device:
**Settings → Privacy & Security → Developer Mode**, toggle on, reboot. Xcode
will usually prompt you, but if the device does not appear as a valid
destination this is why.

## Step 5 — Copy the game data onto the device

**Launch the app once before copying anything.** On first launch it creates an
empty `Documents/gamedata`, and that is what makes the folder appear in the
Files app. Before that first launch there is nothing to copy into. The app will
start, find no data, and stop — that is the expected outcome of this step.

Then, in the **Files** app, go to **Browse → On My iPad → pikpik** and copy the
*contents* of your extracted disc into `gamedata`:

```
On My iPad/pikpik/
  gamedata/
    dataDir/
    ...
```

The data is not provided and cannot be; see [GAME_DATA.md](GAME_DATA.md) for
what it is and which revision is required (`GPIE01_01`, USA Rev 1).

It is roughly 640 MB over a slow path. Expect it to take a while and expect the
Files app to be vague about progress. Copying from a Mac over a cable, or from
an external drive plugged into the device, is usually faster than iCloud Drive.

Saves and `settings.ini` land in the same `Documents/` directory, which is also
how you get a save file back off the device.

## Step 6 — Run it

Press Run again. Watch the Xcode console: `main.m` force-enables `PC_VERBOSE`,
so the engine prints the paths it resolved at startup. If something is wrong
with the data, it says so there rather than failing mysteriously later.

**Controls.** Both schemes are live at once:

- **Touch** — an on-screen control overlay (`pc/src/pc_touch_pad.cpp`).
- **A game controller** — pair it in iPadOS Settings first. SDL's
  GameController layer rides iOS's own GameController framework, so anything
  the OS recognises maps positionally with no per-pad configuration here.

**Useful scheme settings.** Product → Scheme → Edit Scheme → Run → Arguments →
Environment Variables:

| | |
|---|---|
| `PC_SKIP_INTRO=1` | Boot straight to gameplay. You will want this while iterating |
| `PC_GAME_ROOT=…` | Override the `Documents/gamedata` default |
| `PC_VERBOSE=1` | Already forced on in `main.m` during bring-up |

[CONFIG.md](CONFIG.md) documents all ~65 of them.

---

## Notes worth reading before you lose an afternoon

**Xcode does not know about the CMake build.** After changing anything in `pc/`
or in the game tree, re-run `./build_ios.sh` *before* pressing Run. Otherwise
Xcode happily links yesterday's archives and you debug a bug you already fixed.
This catches everyone at least once.

**The Info.plist is merged, not replaced.** The target has both
`GENERATE_INFOPLIST_FILE = YES` and `INFOPLIST_FILE = Info.plist`, so Xcode's
generated keys and the ones in `app/Info.plist` are combined. The hand-written
file exists because `UIFileSharingEnabled` has no `INFOPLIST_KEY_` equivalent —
the generated-plist route alone cannot set it, and without it the game data
cannot reach the device at all.

**The unexported-symbol list lives in two places.** The game replaces
`operator new` and makes `operator delete` a no-op; letting the system
frameworks bind those crashes their initialisers. The desktop build hides them
at link time in `pc/CMakeLists.txt`. The iOS build cannot, because CMake only
*archives* on iOS and never links — so the same list is repeated in the Xcode
target's `OTHER_LDFLAGS`. **Add a symbol to one and you must add it to the
other**, or you get a crash inside a system framework's initialiser that looks
like anything but a symbol-visibility problem.

**Backgrounding is handled, but not indefinitely.** The frame loop parks and
audio pauses on `SDL_APP_WILLENTERBACKGROUND`; both resume on return. What is
*not* handled is iOS reclaiming the app's memory after a long absence — and
because the game only writes the memory card at the end of an in-game day, that
loses the day. Treat a long backgrounding as a lost session.

**Local networking.** The optional save-sync feature talks to a server on your
own LAN over plain HTTP, which App Transport Security blocks by default. The
`Info.plist` sets `NSAllowsLocalNetworking` — LAN destinations only, with ATS
still enforced everywhere else — plus `NSLocalNetworkUsageDescription`, without
which iOS denies local-network access silently. A server reached over anything
that is not the LAN (a VPN address in the CGNAT range, say) is *not* covered;
put it behind TLS and use an `https://` URL rather than widening the key. Off
unless you configure it — see [SAVE_SYNC.md](SAVE_SYNC.md).

## Troubleshooting

**"Signing for 'pikpik' requires a development team"** — Step 4. You have not
selected your team.

**"Failed to register bundle identifier"** — you left it as
`com.example.pikpik`. Change it to your own reverse-DNS.

**"Unable to install… device not eligible" / device missing from the
destination menu** — Developer Mode is off on the device (Step 4), or the
device's iOS version is below the 16.4 deployment target.

**Untrusted Developer on launch** — trust the certificate on the device, Step 4.

**`ld: library 'pikpik' not found`** — `./build_ios.sh` has not been run, or it
failed. The Xcode target links `-lpikpik -lpikmin_core -lpikmin_pc` out of
`build-ios/lib`, which only CMake produces.

**`iOS static SDL2 not found`** from CMake — `build_ios.sh` builds SDL2 before
the engine; if you invoked CMake directly, run the script instead.

**App launches, then exits** — almost always the game data. Read the Xcode
console: the startup lines say exactly which paths were tried. The most common
mistake is copying `dataDir` itself into `Documents/` rather than into
`Documents/gamedata/`.

**Renders wrong, or crashes in Metal** — capture a frame and read the comments
in `pc/src/pc_gx_stub.cpp` and `pc/src/pc_window.mm`. The renderer's non-obvious
behaviour is documented inline, including several cases where the intuitive
explanation turns out to be the wrong one.
