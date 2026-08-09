# 2D / UI (P2D) rendering — reference for the port

How Pikmin's 2D UI (menus, dialogs, HUD) reaches the screen, how the GX shim
(`pc/src/pc_gx_stub.cpp` → `pc/src/pc_window.mm`) models it, and where the model
is still incomplete. This is the subsystem behind the title Start/Options menu,
the memory-card dialogs, and every `.blo`-driven screen. It is a real port, not a
set of one-line shim patches — this doc exists so the remaining work is grounded
in what the hardware actually does.

## GX color-channel model (the part the shim under-implements)

A vertex's lit "rasterized color" (RAS, split RASC/RASA) is NOT simply the vertex
color. `GXSetChanCtrl(chan, enableLight, ambSrc, matSrc, lightMask, diffFn,
attnFn)` selects, per channel:

- `matSrc = GX_SRC_REG (0)`  → material color/alpha comes from the **register**
  set by `GXSetChanMatColor` (and `GXSetChanAmbColor`).
- `matSrc = GX_SRC_VTX (1)`  → material color/alpha comes from the **vertex**
  (`GX_VA_CLR0`).

So a draw can bind white vertex colors AND carry its real color/alpha in the
register. **P2DPicture does exactly this**: white vertices + `matSrc=GX_SRC_REG`
+ `GXSetChanMatColor(255,255,255, mAlpha)` — the fade/transparency lives in the
register alpha ([P2DPicture.cpp `setTevMode`](../src/plugPikiYamashita/P2DPicture.cpp)).

Shim state: `s_chan0_matsrc` (from `GXSetChanCtrl`), `pc_gx_chan_mat[4]` (from
`GXSetChanMatColor`). In `pc_cap_emit_vertex`, the baked rasterized **alpha**
(`rasa`) = `pc_gx_chan_mat[3]` when `matSrc==REG`, else the vertex alpha
`s_cap.clr[3]`. **Known partial:** RGB is still always taken from the vertex path
— fine only because every REG case seen so far has white register RGB. A material
that puts a non-white color in the register (not just alpha) will render wrong
until RGB is sourced the same way.

**Stale-state hazard:** `matSrc` is channel state that persists across draws. A
draw that does not call `GXSetChanCtrl` inherits the previous draw's value. This
bit us: flat `GX_PASSCLR` draws inherited `REG` from a prior P2DPicture. Mitigated
by resetting `s_chan0_matsrc = GX_SRC_VTX` inside `GXSetTevOp(GX_PASSCLR)` (a
PASSCLR preset outputs the vertex-sourced flat color by definition). This is a
patch over a deeper gap: the shim does not fully replay GX channel state per draw.

## TEV model: record-and-evaluate (v21)

The shim RECORDS the full TEV state — all 8 stages' color/alpha args, ops
(bias/scale/clamp/output register), per-stage order (texmap/texcoord/channel),
konst selects, the C0–C2 registers and K0–K3 konst colors — and packages it per
draw segment into a `PcTevProg` ([pc/src/pc_tev_prog.h](../pc/src/pc_tev_prog.h)).
One Metal fragment ubershader (`fmain_tev`) evaluates the REAL GX stage
equation against a live register file:

    out = d [±] ((1-c)·a + c·b) + bias, ×scale, clamp → out_reg
    (compare ops 8–15: out = d + ((a OP b) ? c : 0), R8/GR16/BGR24/RGB8)

The pixel is the color/alpha of the registers written by the final stage. Up to
8 textures bind per segment (one per sampling stage); `GX_TEXMAP_NULL` stages
see texel=0 (GX truth), and a texmap with no texture on our side degrades to a
neutral white texel. Vertices carry only honest GX inputs: clip pos, the lit
rasterized channel color, and generated `GX_TEXCOORD0/1`. Multi-stage
P2DPicture cascades, the 7-stage title background, and env-mapped materials
(stage 1 + NRM texgen) all evaluate exactly — there is no multi-stage fallback
anymore. Presets (`GXSetTevOp`) expand per the SDK's own GXTev.c; note real
`GX_REPLACE` outputs pure `tex` (not `tex·ras`).

**ARG-ENCODING TRAP (cost a session; do not re-learn).** The hardware
`GXTevColorArg` encoding INTERLEAVES color and alpha registers: `CPREV=0,
APREV=1, C0=2, A0=3, C1=4, A1=5, C2=6, A2=7, TEXC=8, TEXA=9, RASC=10, RASA=11,
ONE=12, HALF=13, KONST=14, ZERO=15`. This repo's `GXEnum.h` names them
`C0=2, C1=3, C2=4, A0=5, A1=6, A2=7` — the NAMES are shifted, but the decomp's
call sites carry binary-authentic VALUES, so runtime values must be decoded per
the hardware table above (the shader does). `GXTevAlphaArg` (`APREV=0, A0=1,
A1=2, A2=3, TEXA=4, RASA=5, KONST=6, ZERO=7`) matches hardware as-is.

**Enum-mismatch bug (was THE white-box cause; fixed v20, kept for the trail).**
`GXTevMode` is `MODULATE=0, DECAL=1, BLEND=2, REPLACE=3, PASSCLR=4` (GXEnum.h);
the old shim's `GXSetTevOp` switch was off by one, so REPLACE decoded as
PASSCLR → texture discarded → the flat-white window frames/glass shine. Same
lesson family as the arg-encoding trap: verify the GX contract, and don't trust
even the local headers without checking hardware truth (Dolphin source is the
reference).

## P2D pane render pipeline

Screen (`.blo`) → draw tree:

```
zen::DrawMenu::draw → DrawScreen::draw → P2DScreen::draw → P2DPane::draw (recursive)
                                                              └─ drawSelf() per pane
```

`P2DScreen::draw` ENDS by setting `GX_PASSCLR` + `matSrc=GX_SRC_VTX` as leftover
cleanup state — anything drawn afterward without its own TEV setup inherits it.

Pane `drawSelf` overrides and the GX state each establishes:

- **`P2DPane`** (base) — draws nothing (just a pos-matrix for a callback).
- **`P2DPicture`** — textured. `setTevMode`: **simple `REPLACE`** iff
  `mTextureCount==1 && mAlpha==255 && mBlack==0 && mWhite==0xFFFFFFFF`; otherwise
  a **multi-stage cascade**: stage0 `(TEXC,0,0,0)`; a `lerp(mBlack, mWhite, tex)`
  stage when black/white are non-default (`C0=mBlack`, `C2=white` via `TEVREG2`);
  an alpha-fade stage `(ZERO,APREV,RASA,ZERO)` when `mAlpha!=255`. Always sets
  `matSrc=GX_SRC_REG` + `GXSetChanMatColor(…,mAlpha)`. Never emits `PASSCLR`.
- **`P2DWindow`** — a bordered panel: the **frame/corners** draw via
  `P2DWindowTexture::draw` → `setTevMode` = **`REPLACE` + textured** (shows the
  frame graphic); the **fill** draws via `drawContents` = **`PASSCLR`,
  `TEXMAP_NULL` (no texture)**, colored by 4 per-vertex corner colors
  (`mTLCornerColour…`, `matSrc=GX_SRC_VTX`).
- **`P2DTextBox`** — glyph quads from the font texture.

`PC_PANETRACE` (added in `P2DPane::draw`) prints each visible pane's 4-char tag +
640×480 bounds, to map a captured draw back to a `.blo` pane.

## Anatomy of the title Start/Options menu (`screen/blo/m_select.blo`)

From an F9 capture (`pc_capture_segs.txt`), segments back-to-front:

| seg | what | TEV / notes |
|-----|------|-------------|
| 00–03 | 3D title scene (grass, plants, logo) | textured, opaque |
| 04 | ground quad | **`tex=-1` UNTEXTURED** → renders dark (should be the grass photo) |
| 06 | dim overlay ("black underlay") | untextured, `a=RASC`, `matSrc=REG`, register α=0.25 → translucent when REG-alpha honored |
| 09, 12 | glass panel fill / label | `TEXC`, register α<1 → translucent |
| 10 (tex13), 14 (tex15) | **glass shine / frame** | **`GX_PASSCLR` (`d=RASC`), texture bound but IGNORED → flat white.** Should show the metallic-glass graphic. This is the "white box." |
| 11, 13 (tex10) | cursor orbs | vertex-colored `(0.31,0,1)`/`(1,0,1)` — wrong color |

Native reference (user, side-by-side): grassy scene + PIKMIN logo + a **dark
translucent glass** window with a metallic frame + screws; "Start" orange,
"Options" cyan.

## Open port work (2D UI), grounded

1. **Glass shine/frame white (seg 10/14) — root cause found: the `GXSetTevOp`
   enum-mismatch above.** These are `GX_REPLACE` textured draws that the shim
   decoded as `PASSCLR`. Fixed by correcting the case numbers; verify the frame/
   shine now sample `0xb9de06080` (blended, using the texture's alpha) instead of
   painting white.
2. **Untextured 3D ground (seg 04, `tex=-1`).** Texture-bind gap in the 3D
   material path — the ground's texture isn't reaching the draw.
3. **Cursor / text colors.** Cursor orbs `(0.31,0,1)`; "Start" orange→magenta —
   konst/vertex color sourcing.
4. ~~True multi-stage / multi-texture TEV.~~ **DONE (v21)** — the recorded-program
   ubershader evaluates every stage exactly; see the TEV section above.
5. **Per-draw GX channel-state replay.** `matSrc` and channel colors still
   persist as globals with reset hacks (PASSCLR→matsrc=VTX); TEV state itself is
   now snapshotted per draw, so this remains only for the channel/lighting side.

## Diagnostics (env-gated unless noted)

- **F9** (in-window key): dumps `pc_capture.png` + `pc_capture_segs.txt` for the
  exact on-screen frame. The per-seg line carries: `tex`, stage-0 TEV args,
  `matsrc`, blend `src/dst`, `atest`, NDC bounds, and baked `A`/`B` endpoints.
- `PC_PANETRACE` — pane tag + bounds (`P2DPane::draw`).
- `PC_MATTRACE` — PVW material texmap/stage layout (`dgxGraphics::compileMaterial`).
- `PC_TEV_TRACE` — distinct TEV arg combos → endpoints. `PC_DUMP_TEX` — decoded
  textures to `tex_NNN.png` + mean brightness. `PC_DUMP_SEGS=<frame>` /
  `PC_DUMP_FRAMES=<frames>` — headless equivalents of F9 (frame-timing is
  unreliable for reaching a specific menu; F9 is the reliable path).
