# pikpik

A native **macOS (Metal)** and **iOS** source port of *Pikmin* for the Nintendo
GameCube, built on the [projectPiki decompilation](https://github.com/projectPiki/pikmin).

Runs on Apple Silicon. Renders through Metal, mixes audio by emulating the
GameCube's DSP, emulates the memory card for saves, and — on iPad and iPhone —
takes touch input. Status: **playable start to finish.**

![Day 1 on The Impact Site, running on an iPad with the port's on-screen touch
controls visible](https://raw.githubusercontent.com/magikarp/assets/main/screenshot.jpg)

## What works

| | |
|---|---|
| **Rendering** | The GameCube's TEV pipeline translated to Metal, at your display's resolution and refresh rate |
| **Audio** | Full DSP emulation — music, effects, streamed sequences and the reverb bus, not a stub |
| **FMV** | The game's HVQM4 videos play |
| **Saves** | Memory-card emulation backed by host files. Existing `.gci` saves can be imported |
| **Gamepads** | Anything SDL recognises, on both platforms. On iOS, pair it in Settings and it maps positionally — no per-pad setup |
| **Keyboard** | Fully rebindable through `keybindings.ini` |
| **Touch** | An on-screen controller on iPhone and iPad, live alongside a gamepad |
| **HD texture packs** | Optional. Point `PC_HD_TEXTURES` at a Dolphin-format pack; a texture with no replacement falls through to the original. BC7 packs need no CPU decoder on M-series hardware |
| **Save sync** | Optional, and **pre-release**: the client is here and works, but the small server it talks to has not been published yet. One save shared across your Mac, iPad and iPhone, with a lease so two devices cannot both edit it |

**Widescreen is not supported.** The game renders at its original 4:3 aspect and
is pillarboxed. This is not on the roadmap.

Everything else is the game as it shipped — this is a port, not a remaster, and
where the original had a quirk the port keeps it.

> **This repository contains no game code and no game assets.**
> It is the port layer. `setup.sh` fetches the decompiled sources from their own
> upstream project at build time, and you supply the game data yourself from a
> disc you own. Read [docs/LEGAL.md](docs/LEGAL.md) before you do anything else.

---

## Credit first

The hard part of this was not done here. projectPiki's 100%-matching
decompilation is ~368,000 lines of reverse-engineered source that recompiles to
the original GameCube binary exactly; this project is ~10,000 lines of changes
on top of it plus a host layer. See [CREDITS.md](CREDITS.md).

## What you need

| | |
|---|---|
| Hardware | A Mac with Apple Silicon (M1 or later) |
| macOS | 12.0 or later |
| Tools | Xcode Command Line Tools, `cmake`, `git`, Python 3 |
| SDL2 | `brew install sdl2` (macOS only — the iOS build fetches its own) |
| Game data | An extracted `GPIE01_01` disc filesystem — *Pikmin*, USA Rev 1. Not provided. See [docs/GAME_DATA.md](docs/GAME_DATA.md) |
| For iOS | Xcode, and an Apple Developer account to run on a device |

## Build it — macOS

```bash
git clone https://github.com/magikarp/pikpik.git && cd pikpik
./setup.sh
```

`setup.sh` fetches the decompilation into `vendor/pikmin` at a pinned commit and
applies this port's patches. Then point the build at your game data and go:

```bash
ln -s /path/to/extracted/disc build-m1/gamedata
./build_mac.sh && ./bundle_mac.sh && open dist/pikpik.app
```

Full detail, including what to do when it goes wrong:
[docs/BUILDING_MACOS.md](docs/BUILDING_MACOS.md).

## Build it — iOS / iPadOS

```bash
./setup.sh
./build_ios.sh
open app/pikpik.xcodeproj
```

Set your own team and bundle identifier in Xcode's Signing & Capabilities, then
Run. Copy the game data into the app's `Documents/gamedata` through the Files
app. Full detail: [docs/BUILDING_IOS.md](docs/BUILDING_IOS.md).

## How it is put together

```
pc/                the port layer — everything that replaces GameCube hardware
core-additions/    new source files this port adds to the game tree
patches/           this port's changes to the decompiled sources, as diffs
app/               the iOS Xcode shell project
vendor/pikmin/     the decompilation — fetched by setup.sh, never committed
```

The engine is the decompiled game, unchanged in shape. Everything the GameCube
used to provide — the Flipper GPU, the DSP, the disc drive, the memory card, the
controller — is reimplemented behind the same function signatures in `pc/`, so
the game code calls `GXSetTevOp` and a Metal pipeline comes out the other end.

[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) is the tour.

## Documentation

| | |
|---|---|
| [docs/LEGAL.md](docs/LEGAL.md) | What is and is not distributed, and why. Read this one |
| [CREDITS.md](CREDITS.md) | Whose work this is built on |
| [docs/GAME_DATA.md](docs/GAME_DATA.md) | What the game data is and where it goes |
| [docs/BUILDING_MACOS.md](docs/BUILDING_MACOS.md) | The macOS build, in full |
| [docs/BUILDING_IOS.md](docs/BUILDING_IOS.md) | The iOS build, signing, and getting data onto a device |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | How the port layer is organised |
| [docs/CONFIG.md](docs/CONFIG.md) | `settings.ini`, `keybindings.ini`, and every `PC_*` debug switch |
| [docs/PATCHES.md](docs/PATCHES.md) | Working on the game tree, and regenerating the patch series |
| [docs/RENDERING_2D_UI.md](docs/RENDERING_2D_UI.md) | How the game's 2D and UI layer draws |
| [docs/SAVE_SYNC.md](docs/SAVE_SYNC.md) | Optional save sync between your own devices |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Rules for contributions — including some hard ones about what not to send |

## Licence

MIT for everything in this repository — see [LICENSE](LICENSE), and
[NOTICE](NOTICE) for the provenance of every component of a working build.

*PIKMIN* and *NINTENDO* are trademarks of Nintendo. This project is not
affiliated with, endorsed by, or approved by Nintendo.
