# Architecture

The organising idea is that **the game does not know it has been ported.**

The decompiled game code keeps its shape. Everything the GameCube used to
provide — the Flipper GPU, the DSP, the disc drive, the memory card, the
controller ports, the OS — is reimplemented behind the same function
signatures, so game code calls `GXSetTevOp` or `CARDWrite` exactly as it did and
a Metal pipeline or a host file comes out the other end.

That constraint is the reason the patch series is 10,000 lines rather than
100,000, and the reason a divergence from the original can usually be localised
to one shim rather than argued about.

```
   the decompiled game  (vendor/pikmin — fetched, patched)
            │
            │  the Dolphin SDK surface: GX, AI, AR, DVD, CARD, OS, PAD, VI
            ▼
   the port layer  (pc/)
            │
            ▼
   Metal · SDL2 · CoreAudio · host filesystem
```

## The port layer, file by file

| | |
|---|---|
| `pc_gx_stub.cpp` | The GX graphics API. Translates the GameCube's register-machine GPU — TEV combiner stages, texture environment, blend and Z modes, vertex descriptors — into Metal pipeline state and draw calls. The largest and most intricate file here |
| `pc_tev_prog.h` | The TEV → shader translation: what the fixed-function combiner stages become in Metal Shading Language |
| `pc_window.mm` | The glass. SDL window plus `CAMetalLayer`, presented once per VI retrace. Also owns texture upload, including the BC7 path for HD texture packs |
| `pc_sdk_shims.cpp` | Everything else in the SDK surface the game links against — OS threads and mutexes, DVD reads, ARAM, VI timing, PAD input |
| `pc_dsp.cpp` | The DSP. Not a stub: the GameCube's audio DSP ran Nintendo's Zelda-family microcode, and this reimplements the mailbox protocol, the voice-parameter blocks, AFC/ADPCM decoding, resampling, the biquad and the reverb bus |
| `pc_audio.cpp` | SDL2 audio output beneath the DSP, and the AI DMA seam the game drives it through |
| `pc_card.cpp` | Memory-card emulation backed by host files. The real CARD library is in the decomp but sits on EXI hardware, so it is replaced rather than driven |
| `pc_paths.cpp` | Where game data, saves, and configuration live on each platform. Allocation-free by necessity — it runs before the game's arena exists, and the game replaces `operator new` |
| `pc_config.cpp` | `settings.ini` and `keybindings.ini`. [CONFIG.md](CONFIG.md) |
| `pc_touch_pad.cpp`, `pc_touch_draw.mm` | The iOS on-screen controller, and its Metal rasteriser |
| `pc_ios_main.cpp` | The iOS entry point bridge — SDL's UIKit backend must own `UIApplicationMain` |
| `pc_sync.cpp`, `pc_sync_net.mm` | Optional save sync between your own devices. Off unless configured. [SAVE_SYNC.md](SAVE_SYNC.md) |
| `pc_input_rec.cpp` | Input recording and replay, keyed to the simulation tick. The backbone of headless testing |
| `pc_crash.cpp` | Self-reporting crash backtraces |
| `pc_uninit.cpp` | Instrumentation for the uninitialised-read audit |

## Three things worth understanding before changing anything

**The build flags are load-bearing.** `pc/CMakeLists.txt` carries a long,
heavily commented set — `-funsigned-char`, `-fwrapv`,
`-fno-delete-null-pointer-checks`, `-ftrivial-auto-var-init=zero` and more. None
is stylistic. Each one either matches a documented CodeWarrior behaviour or
defuses a class of undefined behaviour that clang would otherwise exploit. The
comments say which; read them before removing one.

**`-O0` is the reference build, not a debug convenience.** `-O2` is available
behind `PC_O2=ON` and is treated as something to be *earned* per UB class rather
than switched on: an optimising compiler turns several kinds of latent decomp
UB into real miscompiles, and the way to find out which is an A/B against the
`-O0` build, not optimism. If you turn it on and something misbehaves, suspect
the flag before you suspect your change.

**Testing is replay, not unit tests.** `pc_input_rec.cpp` records controller
input against the simulation tick and replays it deterministically from a
matching save snapshot. That is how a change is shown not to have broken
anything — a recording plays through and the frames match, or it does not. There
is no test suite in the conventional sense and it is not clear one would help.

## Where the answers are

In the comments. The port layer is deliberately comment-heavy, and the comments
are load-bearing: they explain why a shim behaves the way the hardware did, why
a particular flag is not negotiable, and in several places why the obvious
explanation for some behaviour is wrong. That knowledge mostly does not exist
anywhere else.

Read the surrounding code before forming a theory. For questions about the
decompiled game code itself rather than the port, the upstream project
([projectPiki/pikmin](https://github.com/projectPiki/pikmin)) is the place to
ask.
