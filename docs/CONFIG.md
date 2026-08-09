# Configuration and debug switches

Two layers: **`.ini` files** for things a player changes, and **environment
variables** for development. Environment variables win, so every headless
recipe in this repo keeps working regardless of what the `.ini` files say.

    environment variable  >  .ini file  >  built-in default

## Where things live

User data (settings, keybindings, saves) is **portable when the build is
loose**: a plain binary keeps them in its own directory, so a development tree
is one place rather than two. A bundled `.app` uses the macOS-conventional
location instead — writing inside a `.app` invalidates its code signature, and
bundles are meant to be read-only. That is the only reason the two differ.

| Running | User data goes to |
|---|---|
| `build-m1/Pikmin` (loose) | `build-m1/` — `settings.ini`, `keybindings.ini`, `save/` |
| `dist/Pikmin.app` (bundled) | `~/Library/Application Support/Pikmin/` |
| Either, with `PC_CONFIG_DIR` set | that directory |

The startup log says which one is in use:
`[PC] user data: <path> (portable — beside the executable)`.

`PC_CARD_DIR` still overrides the save location on its own, which is what the
headless tests use so they never touch a real save.

| What | Path |
|---|---|
| Game data | `$PC_GAME_ROOT`, else next to the executable — see `pc/src/pc_paths.cpp` |

Both `.ini` files are written with their defaults **and comments** on first
run, so they are discoverable without reading this document. Delete either to
regenerate it. Annotated copies live beside this file:
[`settings.ini.example`](settings.ini.example),
[`keybindings.ini.example`](keybindings.ini.example).

Game data resolution order: `$PC_GAME_ROOT` → `<exe>/../Resources/gamedata`
(inside a bundle) → `<exe>/../Resources/gamedata.path` (a one-line text file
holding a path — a symlink there makes the app unsignable) → `<exe>/gamedata` →
`./gamedata`.

## settings.ini

| Section | Key | Default | Meaning |
|---|---|---|---|
| `video` | `scale` | `2` | Window size as a multiple of 640x480, clamped 1..6 |
| `video` | `fullscreen` | `0` | Start fullscreen (letterboxed to 4:3) |
| `audio` | `enabled` | `1` | Master jaudio switch |
| `input` | `gamepad` | `1` | `0` ignores controllers; keyboard stays live |

## keybindings.ini

`action = key[, key ...]`, using SDL key names. Empty unbinds. Actions:

- buttons — `a b x y z l r start`
- stick — `up down left right`
- C-stick — `cup cdown cleft cright`
- D-pad — `dup ddown dleft dright` (unbound by default)

Unknown key names are reported on stderr rather than silently dropped.

## Runtime keys

| Key | Action |
|---|---|
| `Cmd-F`, `F11`, `Alt-Enter` | Toggle fullscreen |
| `Escape` | Leave fullscreen. **Does not quit** — it used to, and was far too easy to hit |
| `Cmd-Q`, close button | Quit |
| `F9` | Grab the current frame to PNG |
| `F10` or `G` | Capture the frame's GPU work to `pikmin.gputrace` (open in Xcode) |

## Environment variables

Harvested from source; the file column is where to look when a description here
is not enough.

### Paths and core

| Variable | Effect | Source |
|---|---|---|
| `PC_GAME_ROOT` | Disc-filesystem root (the directory containing `dataDir`). Empty counts as unset | `pc_paths.cpp` |
| `PC_CONFIG_DIR` | User-data directory (settings, keybindings, saves). Overrides the portable/bundled choice | `pc_paths.cpp` |
| `PC_CARD_DIR` | Memory-card root alone. Used by headless tests so they never touch real saves | `pc_card.cpp` |

A Dolphin/GameCube `.gci` can be imported straight into a card dir with
`tools/gci_import.py <file.gci> [card-dir]` — useful for jumping to late-game
state (a final-boss save) instead of playing there. The `.gci` payload is
byte-for-byte our `<name>.bin`; only the `.stat` sidecar is rebuilt.
| `PC_NO_CARD` | Report NOCARD everywhere | `pc_card.cpp` |
| `PC_CARD_SELFTEST` | Run the card self-test (33 checks) and exit, without booting | `sysBootup.cpp` |
| `PC_VERBOSE` | Verbose shim logging | `pc_sdk_shims.cpp` |
| `PC_UNINIT_PROBE` | Count the `-Wconditional-uninitialized` fall-throughs the run actually reaches. Reports each site's first hit immediately and a per-site total at exit or on SIGTERM. Measures PATH TAKEN, not uninitialized-ness, so a fixed site still counts — useful as a standing regression check | `pc_uninit.cpp` |
| `PC_AUDIO` | Overrides `audio.enabled` | `pc_config.cpp` |
| `PC_NO_PAD` | Overrides `input.gamepad` (presence disables pads) | `pc_config.cpp` |

### Driving the game without hands

| Variable | Effect | Source |
|---|---|---|
| `PC_AUTO_START` | Auto-press through boot into gameplay | `pc_sdk_shims.cpp` |
| `PC_AUTO_TITLE` | Only press to the title, then idle so attract mode runs | `pc_sdk_shims.cpp` |
| `PC_SKIP_INTRO` | Fast-forward the unskippable opening cinematic chain | `moviePlayer.cpp` |
| `PC_WALK` | Scripted movement, for headless capture runs | `pc_sdk_shims.cpp` |
| `PC_MOVIE=<0..5>` | Boot straight into a `.h4m` movie. `0-3` tutorials, `4` normal ending, `5` happy ending | `gameflow.cpp` |
| `PC_UNCAP` | Skip the frame wait — free-run for testing | `pc_sdk_shims.cpp` |
| `PC_FIXED_DT[=<seconds>]` | Pin the simulation timestep instead of deriving it from `OSGetTick()`. Default `1/30`, the value a locked 30 fps already produces. Everything frame-timed reads `gsys->getFrameTime()` — including the `PC_LOGO_HOLD`/`PC_BANNER_HOLD` countdowns — so a scripted run advances by frame count rather than by machine speed. Added for the -O0/-O2 A/B (docs/ARCHITECTURE.md); **note the scripted input in `PADRead` is still keyed to `VIGetRetraceCount()`, which is a wall-clock 60 Hz thread**, so this alone does not make two different builds line up | `system.cpp` |
| `PC_INPUT_REC=<file>` | Record the session's input, one entry per **sim tick** (a `PADRead` call). Only changes are stored and each is flushed immediately, so a recording survives the crash it captured. Captures keyboard and controller alike — `pc_window_get_pad` has already merged them | `pc_input_rec.cpp` |
| `PC_INPUT_PLAY=<file>` | Replay a recording, overriding all real and scripted input. Defaults `PC_FIXED_DT` on, and warns if the save state differs from the one recorded | `pc_input_rec.cpp` |
| `PC_SOUND_TRACE` | Log each distinct (sound id, failing lookup) once when `Play_1shot` bails — `InstRead`, `WaveidToWavegroup`, `GetSoundHandle` or the logical-channel grab returning null. Distinguishes a systematic gap in the loaded wave groups from a one-off | `oneshot.c` |
| `PC_DUMP_TICKS="900,1800"` | Like `PC_DUMP_FRAMES` but indexed on the sim tick, so the same tick is the same moment in every build. **Use this, not `PC_DUMP_FRAMES`, for any A/B across builds** — presents are suppressed during asset loads, so a faster build reaches a given frame number at a different point in the game | `pc_window.mm` |
| `PC_PERF` | Per-frame timing split (logic / submit / render / retrace), 120-frame averages | `pc_sdk_shims.cpp` |
| `PC_LOGO_HOLD`, `PC_BANNER_HOLD`, `PC_NO_BANNER` | Shorten or skip boot holds | `ninLogo.cpp`, `newPikiGame.cpp` |

### Audio and the DSP renderer

| Variable | Effect | Source |
|---|---|---|
| `PC_AUDIO_STATS` | DMA frames produced, callback underruns, ring fill | `pc_audio.cpp` |
| `PC_DUMP_WAV=<path>` | Record the DAC output to a WAV (used for the Dolphin A/B) | `pc_audio.cpp` |
| `PC_DSP_SURVEY` | Voice/bus/FX census + DSP channel pool occupancy | `pc_dsp.cpp` |
| `PC_DSP_TYPEHIST` | Voice-frames per source type, and how many carry volume | `pc_dsp.cpp` |
| `PC_DSP_LOOPSTAT` | AFC loop regions and the mid-block restart share | `pc_dsp.cpp` |
| `PC_DSP_AUXAUDIT` | How many voices are audible ONLY via an aux send | `pc_dsp.cpp` |
| `PC_DSP_FIRAUDIT` | Distinct per-voice filter coefficient sets in use | `pc_dsp.cpp` |
| `PC_DSP_MUTE=<type>` | Silence one source type, to identify which path makes a noise | `pc_dsp.cpp` |
| `PC_DSP_MUTE_CH=<list>` | Silence individual channels. Type muting cannot separate a bad voice from a good one when both use the same decoder | `pc_dsp.cpp` |
| `PC_DSP_MUTE_BASE=<lo>,<hi>` | Silence every voice whose ARAM `baseAddress` falls in a range. This is what localised the menu static to `0x502000`+ | `pc_dsp.cpp` |
| `PC_DSP_BASELOG` | Report each DISTINCT `baseAddress` the mixer renders, once. `PC_DSP_SURVEY` only SAMPLES, so bisecting against its list misses voices | `pc_dsp.cpp` |
| `PC_ARAM_LOG` | Every MRAM→ARAM transfer, so "was this ARAM range ever written, and by how many transfers?" is answerable | `pc_sdk_shims.cpp` |
| `PC_DSP_LINEAR` | Revert the 4-tap polyphase resampler to linear, for A/B | `pc_dsp.cpp` |
| `PC_DSP_NOLOOP` | Never loop a voice | `pc_dsp.cpp` |
| `PC_DSP_TESTTONE` | Emit a distinct tone per channel — capture with `PC_DUMP_WAV` and the recovered frequencies say whether the mixdown, planar split and L/R assignment are all correct | `pc_dsp.cpp` |
| `PC_DUMP_AFC`, `PC_DUMP_AFC_LQ` | Dump a real AFC source from ARAM (`tools/afc_probe.py` decodes it) | `pc_dsp.cpp` |

### Movie playback

| Variable | Effect | Source |
|---|---|---|
| `PC_MOVIE_TRACE` | Picture delivery, GOP/record progress, DVD buffer states, stream buffering | `hvqm_play.c`, `syncstream.c` |
| `PC_MOVIE_DUMP=<path>` | Write delivered pictures as PPM (planar YUV420 → RGB) for visual checking | `hvqm_play.c` |
| `PC_DEMO_TRACE` | Cinematic/demo state | `moviePlayer.cpp` |
| `PC_TEKI_TRACE` | Every enemy created, with type, day, position and generator. Bosses do NOT appear here — they go through BossMgr | `genteki.cpp` |
| `PC_BOSS_TRACE` | Every boss created, with the GenBossID that asked for it. Note GENBOSS_Spider is 0, so a field that failed to load reads as a valid Beady Long Legs | `BossMgr.cpp` |
| `PC_GEN_TRACE` | Why a generator chose to respawn on a day boundary (interval, last spawn day, limit) | `generator.cpp` |

### Renderer diagnostics

| Variable | Effect | Source |
|---|---|---|
| `PC_DIAG_DRAW` | Per-frame segment/vertex/reject counts and clear colour. NOTE it SAMPLES — use `PC_VTXDIAG` for a true maximum | `pc_window.mm` |
| `PC_VTXDIAG` | True per-frame high-water marks for verts and segments, reported **on both sides** of the capture: `[VTX]` is the shim's demand against the capture's capacity, `[PC]` is the same demand against the window's. Overflow of either is a visible bug (dropped geometry / wrong-texture geometry) and warns once unconditionally, with or without this var. Read both lines: `OVERFLOWING` on `[PC]` means the consumer dropped; a `[PC]` peak *below* the `[VTX]` peak means the producer already did. **Run it in the foreground** — a detached replay reports ~19% less load (v95) | `pc_gx_stub.cpp`, `pc_window.mm` |
| `PC_HEAPDIAG` | Report every game heap's **peak** and instantaneous use, after a stage loads and again at exit. Added after day 5 crashed on a full enemy heap (v96). Read the PEAK: `mTotalSize` is only what was live at that instant, and `movie` reads 0% after loading but peaks at 61.7% in play. The at-exit pass is what catches heaps that fill during gameplay rather than during loading. An allocation failure dumps the same table unconditionally. On by default in `probe_session.sh` | `gameCoreSection.cpp`, `sysNew.cpp`, `ayuStack.cpp` |
| `PC_DIAG_SEG` | Per-segment detail, including decoded texture brightness | `pc_window.mm` |
| `PC_DUMP_FRAMES="600,1200"` | Blit those frames to PNG | `pc_window.mm` |
| `PC_DUMP_TEX` | Dump every decoded texture to PNG with its mean brightness | `pc_window.mm` |
| `PC_DUMP_TEXHASH=<dir>` | Dump each distinct texture's raw (tiled) GC bytes + identity, for HD-texture matching — see `docs/ARCHITECTURE.md` | `pc_gx_stub.cpp` |
| `PC_HD_TEXTURES=<dir>` | Root of a Dolphin-format HD texture pack, scanned recursively. Defaults to `<user data>/hd`. A miss falls through to the original texture | `pc_gx_stub.cpp` |
| `PC_DUMP_SEGS` | Dump draw segments | `pc_gx_stub.cpp` |
| `PC_GPU_CAPTURE=<N>` | Write frame N's GPU work to `pikmin.gputrace` | `pc_window.mm` |
| `PC_TEV_TRACE`, `PC_TEXDIAG`, `PC_TEXBIND` | TEV register loads; texture decode; every texture bind | `pc_gx_stub.cpp`, `dgxGraphics.cpp` |
| `PC_LIGHT_TRACE`, `PC_MATTRACE`, `PC_BLEND_TRACE`, `PC_PTCL_TRACE`, `PC_PANETRACE` | Lighting, material registers, blend modes, particles, 2D panes | various |
| `PC_EFB_LOWRES` | Render the scene at GC's 640x480 EFB size and upscale | `pc_window.mm` |
| `PC_NO_TEX`, `PC_NO_DEPTH`, `PC_NO_CULL`, `PC_NO_EFB_COPY`, `PC_NO_SPEC`, `PC_NO_ATTN` | Disable one stage to isolate what a bug depends on | `pc_window.mm`, `pc_gx_stub.cpp` |
| `PC_BLUR` | Scale the blur blend alpha (100 full, 50 half, 0 off) | `mapMgr.cpp` |

### A/B toggles that deliberately restore a bug

These exist so a fix can be compared against the behaviour it replaced. They
make rendering **worse** — that is the point. Do not leave them set.

| Variable | Restores |
|---|---|
| `PC_IA8_SWAP` | The v47 IA8 byte order (v52 established the current one with three independent lines of evidence) |
| `PC_ORIENT_FIX=0` | The broken big-endian particle-orientation decode |
| `PC_SPHERE_NEGY` | The old mirrored sphere-map Y |
| `PC_NRM_FLIP`, `PC_LIGHT_FLIP` | Flipped normals / light direction |
| `PC_NO_LIT_ALPHA` | Unlit alpha (the helmet-glass bug) |
| `PC_NO_MATREG_RGB` | Ignoring the material colour register |
| `PC_BITMSB` | The old particle bitfield interpretation |

## Notes

`pc_paths.cpp` and `pc_config.cpp` **never allocate**. `pc_card_selftest()` runs
from `main()` before `gsys->Initialise()`, and the game replaces `operator new`
with one that returns NULL until the arena exists — and with `-fno-exceptions`,
`std::string` does not throw on that, it memmoves into a null pointer. Keep
fixed buffers in those two files.
