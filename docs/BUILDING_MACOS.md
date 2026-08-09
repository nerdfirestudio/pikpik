# Building on macOS

Target: an Apple Silicon Mac running macOS 12 or later. There is no Intel
build — the port assumes arm64 and Metal throughout, and nobody has tried
otherwise.

## 1. Prerequisites

```bash
xcode-select --install          # Command Line Tools, if you have not already
brew install cmake sdl2
```

Python 3 is used by the icon generator and some tools; the system one is fine.

`sdl2-compat` (Homebrew's default SDL2 these days) works and is what this port
is developed against. It is a shim that dlopens SDL3 at runtime, which
`bundle_mac.sh` accounts for when it assembles the app.

## 2. Get the sources

```bash
git clone https://github.com/magikarp/pikpik.git && cd pikpik
./setup.sh
```

This clones the decompilation into `vendor/pikmin` at the commit recorded in
`setup.sh`, applies `patches/`, and copies in `core-additions/`. It fetches a
single commit rather than the full history, so it is quick.

`vendor/` is gitignored and must stay that way — see [LEGAL.md](LEGAL.md).

Re-runnable at any time. `./setup.sh --check` verifies an existing tree without
touching it; `./setup.sh --force` throws it away and starts over.

## 3. Game data

Not included. See [GAME_DATA.md](GAME_DATA.md). The short version, for a
development build:

```bash
mkdir -p build-m1
ln -s /path/to/extracted/disc build-m1/gamedata
```

## 4. Build

```bash
./build_mac.sh
```

Configures CMake into `build-m1` and builds. About 450 translation units; the
first build takes a few minutes, and later ones are incremental.

Useful variations:

```bash
./build_mac.sh -c                       # wipe the build directory first
BUILD_DIR=build-rel ./build_mac.sh      # build somewhere else
./build_mac.sh -DPC_O2=ON               # optimised — see the note below
./build_mac.sh -DPC_UBSAN=ON            # UBSan, log-and-continue
```

The binary lands at `build-m1/pikpik` and is runnable directly, which is what
you want while developing:

```bash
PC_VERBOSE=1 ./build-m1/pikpik
```

## 5. Bundle it

```bash
./bundle_mac.sh                  # data referenced in place — fast
./bundle_mac.sh --copy-data      # data copied in — ~640 MB, self-contained
open dist/pikpik.app
```

`bundle_mac.sh` lays out the `.app`, copies in the non-system dylibs the binary
links and repoints them at `@executable_path/../Frameworks`, builds the `.icns`
from `assets/icon.png`, records where the game data is, and ad-hoc signs the
result.

Ad-hoc signing is what lets *you* run it. It is not notarisation and it will
not survive being handed to someone else — which is fine, because the app is
not distributable anyway: it needs game data that only its owner can supply.

## Configuration

`settings.ini` and `keybindings.ini` sit next to the executable, or in the user
data directory (`~/Library/Application Support/pikpik`). Annotated examples are
in `docs/settings.ini.example` and `docs/keybindings.ini.example`; every key,
and all ~65 `PC_*` environment switches, are documented in
[CONFIG.md](CONFIG.md).

## When it goes wrong

**`No decompiled game source at …`** — `setup.sh` has not run, or `vendor/` was
deleted. Run it.

**`does not apply cleanly`** from `setup.sh`** — the vendored tree is dirty.
`./setup.sh --force`. If it is clean and still fails, the patches and the pinned
commit have drifted apart, which should not happen in a released tree; open an
issue.

**`SDL2 headers not found in /opt/homebrew/include/SDL2`** — `brew install
sdl2`. If you are on an Intel Mac, Homebrew lives at `/usr/local` and the build
script does not look there; see the note about arm64 at the top.

**Builds, then exits immediately** — almost always game data. Run the binary
directly with `PC_VERBOSE=1` and read the resolved paths it prints at startup.
The usual mistake is pointing at `dataDir` rather than at the directory
containing it.

**Black window, or a crash in Metal** — capture a frame in Xcode, and read the
comments in `pc/src/pc_gx_stub.cpp` and `pc/src/pc_window.mm` around whatever
you are looking at. The renderer's non-obvious behaviour is documented inline,
including several cases where the intuitive explanation is the wrong one.

**Something misbehaves only at `-O2`** — that is the expected direction. `-O0`
is the reference build; `-O2` exposes latent undefined behaviour in a codebase
written for a 1990s compiler. Reproduce at `-O0` before assuming your change is
at fault. See [ARCHITECTURE.md](ARCHITECTURE.md).
