// pc_tev_prog.h — the recorded TEV program that travels with each draw segment.
//
// Design: the GX shim RECORDS the full TEV state (all stages, args, ops,
// registers, konst) instead of algebraically reducing it; the Metal fragment
// shader evaluates the real GX combiner equation per stage:
//     out = d [+/-] ((1-c)*a + c*b) + bias, * scale, clamp -> out_reg
// This struct is BOTH the CPU-side snapshot format and (minus the texhandle
// plumbing) the fragment uniform layout — the renderer hands it to Metal
// verbatim via setFragmentBytes, so the two sides can never drift apart.
// All fields are 32-bit and the float rows are 16-byte groups, matching MSL's
// int4/float4 layout exactly.
#ifndef PC_TEV_PROG_H
#define PC_TEV_PROG_H

#define PC_TEV_MAXSTAGES 8

// ---- draw-stream capacity ------------------------------------------------
// The GX shim (pc_gx_stub.cpp) captures a frame's triangles into segments and
// hands them to the Metal side (pc_window.mm) once per frame, 1:1. PRODUCER AND
// CONSUMER MUST AGREE on the capacity, so it lives here beside the struct
// rather than as a separate literal in each file.
//
// They did NOT agree, and that was the third flicker. v93 and v94 raised the
// shim's caps after measuring overflow, but pc_window.mm kept its own private
// copies at the original 262144 / 2048 and dropped whole segments past them —
// silently, behind a counter only PC_DIAG_DRAW even incremented. Measured:
//
//     session   peak verts   vs the window's 262144
//     day3-2       257031    -5113   (no flicker reported)
//     day4-2       265761    +3617   (flicker reported, on the FIXED build)
//
// Same defect as v93's, one layer downstream, and it explains why the flicker
// went from constant to rare rather than disappearing: the shim fix removed the
// gross discard, leaving only the marginal overshoot on the heaviest frames.
// Its shape is nastier, too — the consumer drops the whole SEGMENT, so once the
// buffer is nearly full a big late segment vanishes while a small one behind it
// still fits, and which objects survive changes with submission order.
//
// Sized against measurement, not a guess: PC_VTXDIAG=1 reports the per-frame
// high-water mark, and day 4 with a boss on screen wants ~341k verts, so 1M is
// ~3x the observed peak. The capture array is BSS, so untouched pages are never
// committed and resident memory tracks use rather than capacity; the Metal ring
// buffer that mirrors it grows on demand for the same reason.
#define PC_CAP_MAX_VERTS 1048576

// Draw-segment capacity. A segment is one run of triangles sharing texture,
// depth mode and TEV program, so segment count tracks MATERIAL variety rather
// than geometry volume — which is why a boss, a single big multi-material
// model, pins this while leaving PC_CAP_MAX_VERTS untouched (measured pinned at
// exactly 2048 on day 4; true uncapped demand 2720).
//
// Overflow on the producer side is worse than the vertex kind: it skips
// creating the segment and then appends the triangle to the PREVIOUS,
// UNRELATED one, so it draws with the wrong texture and render state instead
// of vanishing.
#define PC_CAP_MAX_SEGS 16384

// tex[i][0] (texflag) values:
#define PC_TEVTEX_NONE 0   // stage samples no texture (GX_TEXMAP_NULL): texel = 0
#define PC_TEVTEX_SAMPLE 1 // sample the bound texture for this stage
#define PC_TEVTEX_WHITE 2  // texmap configured but no texture on our side (bind
                           // gap / unpopulated EFB grab): neutral texel = 1

typedef struct PcTevProg {
	int nstages;        // active stages, 1..8
	int lastco, lastao; // output register (0=PREV,1..3=C0..C2) of the final
	                    // color/alpha stage — the pixel the pipeline outputs
	int atest;          // GX alpha compare: (comp0 & 7) | (ref0 << 3), the REAL
	                    // comparator (GXSetAlphaCompare) — particles use
	                    // GEQUAL ref=1 (discard only alpha=0); cutouts GEQUAL 0x80
	float regs[4][4];  // TEV register file at draw time: [0]=PREV(0), [1..3]=C0..C2
	float konst[4][4]; // konst color registers K0..K3
	int cargs[8][4];   // per stage: color args a,b,c,d   (GXTevColorArg)
	int aargs[8][4];   // per stage: alpha args a,b,c,d   (GXTevAlphaArg)
	int cops[8][4];    // per stage: color op, bias, scale, out_reg
	int aops[8][4];    // per stage: alpha op, bias, scale, out_reg
	int tex[8][4];     // per stage: texflag, uvsel (0=coord0, 1=coord1), chansel
	                   // (0: channel NULL/ZERO -> ras=0; 1: COLOR0A0 lit
	                   // diffuse; 2: COLOR1A1 lit specular), kcsel
	int misc[8][4];    // per stage: color clamp, alpha clamp, kasel, texhandle
	                   // (renderer-side texture id for binding; shader ignores)
	int swap[8][4];    // per stage: ras swap table id, tex swap table id (0..3)
	int swaptbl[4][4]; // the 4 swap tables: [table][r,g,b,a] -> source channel
	                   // (0=R 1=G 2=B 3=A). Identity is {0,1,2,3}. Pikmin uses
	                   // {0,0,0,3} (broadcast RED) for its desaturate/fade and
	                   // damage-flash passes (gameCoreSection, damageEffect,
	                   // mapMgr) — with an identity stub those become no-ops.
} PcTevProg;

#endif
