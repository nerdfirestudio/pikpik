# Credits

This port is a thin layer on top of other people's much larger effort. The
proportions are worth stating plainly: the decompilation is roughly 368,000
lines of painstakingly reverse-engineered source. This project's contribution
is about 10,000 lines of changes to it plus a host layer. Without the work
below there would be nothing here to port.

---

## The Pikmin decompilation — [projectPiki/pikmin](https://github.com/projectPiki/pikmin)

Everything this project does rests on projectPiki's decompilation of *Pikmin*
(`GPIE01_01`, USA Rev 1) — a 100%-matching decompilation, meaning the
reconstructed source recompiles to the original GameCube binary instruction for
instruction. That standard is brutally difficult and takes years of collective
work. It is also what makes a port like this *possible* rather than merely
plausible: when the source provably reproduces the original, a divergence in
the port is unambiguously the port's fault, which is the single most valuable
property a project like this can have.

The decompilation is published under CC0 1.0, which asks nothing of anyone
using it — no attribution, no notice, no conditions. This credit is given
because it is deserved, not because it is required.

Pinned here at commit `f193f419`. Please direct questions about the
decompilation itself, and any contribution to it, to that project rather than
to this one.

Its licence is reproduced at
[`third-party/LICENSE-projectPiki-CC0.txt`](third-party/LICENSE-projectPiki-CC0.txt).

## SDL — [libsdl.org](https://libsdl.org)

Window, input, timing, and audio output on both platforms, and on iOS the
`UIApplicationMain` host that the whole app hangs off. Sam Lantinga and the SDL
contributors. zlib licence; text at
[`third-party/LICENSE-SDL2-zlib.txt`](third-party/LICENSE-SDL2-zlib.txt).

SDL is not vendored in this repository — macOS uses your Homebrew install and
`build_ios.sh` fetches and builds the official release tarball — so nothing
here redistributes it. The notice is included anyway.

## The GameCube reverse-engineering community

The port layer emulates hardware this project did not document: the Flipper
GPU's TEV pipeline and register semantics, the AI/ARAM audio path, the DSP's
Zelda-family microcode and its voice-parameter blocks, the AFC/ADPCM sample
formats, the memory-card block layout. Every one of those was worked out over
two decades by people writing emulators, homebrew SDKs, format notes, and wiki
pages — the Dolphin emulator project foremost among them, alongside YAGCD and
the many decompilation projects whose findings circulate between them.

Almost none of that is citable as a single source. It is nonetheless the reason
this port could be written at all, and the reason a bug in it can usually be
diagnosed rather than merely observed.

## Apple

Metal, AVFoundation, CoreAudio, UIKit, AppKit. Used under the Apple SDK terms
that ship with Xcode.

---

## And, unavoidably: Nintendo

*Pikmin* was made by Nintendo EAD — designed under Shigeru Miyamoto, directed
by Masamichi Abe, with music by Hajime Wakai. This project exists because that
game is worth the trouble of keeping runnable.

Nothing here is authorised by Nintendo, nothing here is affiliated with
Nintendo, and nothing here redistributes their work. See
[docs/LEGAL.md](docs/LEGAL.md).
