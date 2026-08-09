# Game data

The build produces an engine with nothing to run. The game's own content —
levels, models, textures, animation, audio banks, video — is about 640 MB of
data that lives on the retail disc, and it is not here and never will be. See
[LEGAL.md](LEGAL.md) section 5.

You supply it, from a disc you own.

## What the port expects

The **extracted disc filesystem**: the directory that *contains* `dataDir`.
Not an ISO, not a compressed image — the unpacked file tree.

```
gamedata/
  dataDir/
    ...
  opening.bnr
  ...
```

The build has to be *Pikmin*, USA Rev 1 — the disc whose identifier is
`GPIE01_01`. That is the revision the decompilation matches 100%, and the only
one this port is built against. The USA original (`GPIE01_00`), the Japanese
releases, the PAL release, and the Player's Choice reissues all differ in file
layout and in code the port depends on; they will not work, and the failure
will not be graceful.

Anything you legally obtain the data with is between you and your jurisdiction.
This project does not tell you how, and ships nothing that helps.

## Where it goes — macOS

Any one of these; the first that resolves wins:

1. `$PC_GAME_ROOT` — an environment variable holding the path. Overrides
   everything, and is the easiest option during development.
2. `<bundle>/Contents/Resources/gamedata` — inside the `.app`, if you built it
   with `./bundle_mac.sh --copy-data`.
3. `<bundle>/Contents/Resources/gamedata.path` — a one-line text file holding a
   path. This is what plain `./bundle_mac.sh` writes, so the app can find data
   that lives elsewhere on your disk. (A symlink here would be simpler but makes
   the bundle unsignable.)
4. `<executable>/gamedata`
5. `./gamedata` in the working directory.

For development the usual arrangement is a symlink beside the build:

```bash
ln -s /path/to/extracted/disc build-m1/gamedata
```

## Where it goes — iOS / iPadOS

Into the app's own `Documents/gamedata`, which you populate through the Files
app after the first launch. The app declares `UIFileSharingEnabled`, so it
appears under **On My iPad → pikpik**.

Copy the *contents* of your extracted disc into a `gamedata` folder there:

```
On My iPad/pikpik/
  gamedata/
    dataDir/
    ...
```

It is a large copy over a slow path; expect it to take a while. Saves and
`settings.ini` land in the same `Documents/` directory, which is also how you
get a save file back off the device.

Details and troubleshooting: [BUILDING_IOS.md](BUILDING_IOS.md).

## Checking it worked

Run the binary directly rather than through the bundle and read the first few
lines:

```bash
PC_VERBOSE=1 ./build-m1/pikpik
```

It prints the resolved game root and the user-data directory at startup. If the
root is wrong or missing you get a clear message there, rather than a puzzling
failure later — the most common mistake by far is pointing at `dataDir` itself
instead of at its parent.
