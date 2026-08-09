# Working on the game tree

This repository does not contain the decompiled game sources. It contains a set
of diffs against them. [LEGAL.md](LEGAL.md) section 4 explains why; this
document explains how to work with the arrangement.

## The layout

```
patches/                 this port's changes to upstream files, as unified diffs
core-additions/          new files this port adds, as ordinary source
vendor/pikmin/           the decompilation — fetched, patched, gitignored
```

`setup.sh` builds `vendor/pikmin` by cloning the upstream project at a pinned
commit, applying `patches/*.patch` in order, and copying `core-additions/` over
the top. That is the whole mechanism.

The pin is a full 40-character SHA in `setup.sh`, not a branch or a tag. It is
the exact tree the patches were generated against, and the only one they are
known to apply to.

## Changing an existing game file

Edit it in `vendor/pikmin`, build, test, then:

```bash
./tools/regen_patches.sh
git diff patches/          # review — this is what you are actually committing
```

Commit the changed patch files. **Never commit anything under `vendor/`.** It
is gitignored, and that is not an accident.

## Adding a new file to the game tree

It does not go in a patch. Put it in `core-additions/`, mirroring the path it
should have in the game tree:

```
core-additions/src/jaudio/bx_pc.c      ->  vendor/pikmin/src/jaudio/bx_pc.c
core-additions/include/pc_uninit.h     ->  vendor/pikmin/include/pc_uninit.h
```

`setup.sh` copies it in, and `regen_patches.sh` will refuse to run if it finds a
new file in the vendored tree with no counterpart there.

The reason for the split is not just tidiness. A new file is entirely your own
work, so it should be readable as source, licensed under this repository's
LICENSE, and reviewable in a pull request — not buried inside a diff against
someone else's code.

## The patch groups

Four, split by path:

| | |
|---|---|
| `0001-headers-host-types-and-lp64` | `include/` — 64-bit-safe types, host shims |
| `0002-audio-and-video-decoders` | `src/jaudio`, `src/hvqm4dec` |
| `0003-system-layer` | `src/sysCommon`, `src/sysDolphin`, `src/sysBootup.cpp` |
| `0004-game-code` | the `src/plugPiki*` game modules |

They touch disjoint paths and each applies independently, so the numbering is
reading order rather than dependency order. If you add a file in a directory no
group covers, `regen_patches.sh` fails rather than silently dropping it — add
the path to a group in both that script and `tools/export_release.sh`'s
equivalent.

## What is in the patches

Overwhelmingly one thing: making a codebase written for a 32-bit big-endian
PowerPC with a 1990s compiler behave identically on 64-bit little-endian arm64
with clang. In rough order of volume —

- **LP64.** A pointer stored in a `u32` is this port's single largest bug
  class. Struct fields, unions, and casts that were exact on GameCube truncate
  silently here.
- **Endianness.** Not just byte swaps on file data: bitfield unions are
  *ordered* by endianness too, which is subtler and has produced some memorable
  bugs.
- **Undefined behaviour the old compiler tolerated.** Falling off the end of a
  non-void function, reading uninitialised stack, out-of-range float-to-int
  casts, taking the address of a temporary. All of these worked by accident at
  `-O0` on hardware and become miscompiles under an optimising clang.
- **Hardware seams.** Small hooks where the game reaches for something the
  GameCube provided and the port layer must answer instead.

Almost none of it changes what the game *does*. Where something had to change
behaviourally, the patch carries a comment saying so.

## Upgrading the pin

Occasionally worth doing, since upstream keeps improving. It is not automatic:

1. Update `DECOMP_COMMIT` in `setup.sh` and `tools/regen_patches.sh`.
2. `./setup.sh --force` and watch which patches fail to apply.
3. Fix them by hand in `vendor/pikmin`, then `./tools/regen_patches.sh`.
4. Rebuild and **replay**, do not merely compile. A patch that applies with
   fuzz onto a changed function is exactly how a silent behavioural regression
   gets in.

Bump the pin as its own commit, with nothing else in it.
