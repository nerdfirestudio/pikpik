// pc_gx_stub.cpp — M2 GX layer: every entry point is a no-op, but vertex
// submission is COUNTED so boot progress is measurable (the prior port
// truth-counter lesson: "no crash" is not a success signal; verts > 0 is).
// M3 replaces this with the real GX -> Metal translation.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

#include "Dolphin/gx.h"
#include "Dolphin/mtx.h"

#include "pc_tev_prog.h"

extern "C" {

// Truth counters, dumped once per second from VI retrace pacing (M3 wires
// them into the real frame loop).
unsigned long pc_gx_frame_begins, pc_gx_frame_verts, pc_gx_frame_draws;
unsigned long pc_gx_dl_indx_ops, pc_gx_posmtx_loads;
int pc_gx_posmtx_maxslot = -1, pc_gx_vtxidx_maxslot = -1;
// st range observed for display-list (mesh) vertices, post-texmtx
float pc_gx_dl_st_min[2] = { 1e30f, 1e30f }, pc_gx_dl_st_max[2] = { -1e30f, -1e30f };
int pc_gx_in_dl, pc_gx_dl_st_texmtx_applied;

// The write-gather pipe: game code streams vertices by writing this global.
// A real store target so those writes are harmless; the vertex COUNT comes
// from GXBegin's nverts argument.
volatile PPCWGPipe GXWGFifo;

// ------------------------------------------------------------ vtx capture
// Immediate-mode vertex capture: GXWGFifo proxy writes (GXFifo.h) land in
// pc_gx_fifo_*; GXBegin's desc/fmt state tells us how to slice the scalar
// stream into vertices. Transform happens CPU-side (proj * posmtx), so the
// Metal side (pc_window.mm) just draws clip-space triangles.
void pc_window_submit_tris(const float* verts, int vertCount, int tex, int depth, const PcTevProg* prog); // pc_window.mm
int pc_window_upload_texture(const unsigned char* rgba, int w, int h, int wrapS, int wrapT, int mip); // pc_window.mm
void pc_window_release_texture(int handle); // pc_window.mm — hands a slot back
int pc_window_copy_handle(const void* dest); // pc_window.mm: live EFB-copy texture for a grab destination
bool pc_gx_is_copy_dst(const void* p); // defined below (GXCopyTex tracking)

// Vertex and draw-segment capacity. PC_CAP_MAX_VERTS / PC_CAP_MAX_SEGS are
// defined in pc_tev_prog.h, WITH the measurements that sized them, because
// pc_window.mm consumes this capture and the two sides must agree — they were
// duplicated literals once, they drifted, and that drift was a flicker bug.
// Overflow of either warns loudly below; it must never be silent again.

#define PC_MAX_ATTRS 16
static struct {
	int attr[PC_MAX_ATTRS];    // GXAttr
	int scalars[PC_MAX_ATTRS]; // scalar count for this attr
	int nattrs;
	int active;                // inside GXBegin?
	int prim;
	int expected, got;         // vertices
	int cursor, filled;        // scalar cursor within vertex
	float pos[3];
	float clr[4];
	float st[2];
	float st1[2]; // GX_VA_TEX1 (second UV set, multi-texture materials)
	float scal[8];             // raw scalars for current attr
	float out[PC_CAP_MAX_VERTS * 16]; // triangulated xyzw rgba st st2 addc
	int outv;
	int wantv;                 // verts the frame ASKED for, uncapped
	int curTex;                // renderer texture handle (-1 none)
	float quad[4][16];         // pending primitive verts
	int quadn;
	int strip_odd;             // tristrip triangle parity (winding restoration)
	float proj[4][4];
	int projPersp;
	float pos34[3][4]; // kept: identity fallback
	float mtxpal[16][3][4]; // GX posmtx palette (ids 0,3..45 -> slot id/3)
	float nrmpal[16][3][4]; // GX normal-matrix palette (GXLoadNrmMtxImm)
	int cur_mtx_slot;
	int vtx_mtx_slot; // per-vertex PNMTXIDX selection (-1 = use current)
	float nrm[3];     // current vertex normal (model space)
	int has_nrm;      // layout carries GX_VA_NRM/NBT
} s_cap;
static float s_ident34[3][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };

// Live GX vertex descriptor state (set by GXSetVtxDesc et al below).
static int s_vtxdesc[26];        // GXAttr -> GXAttrType (0 none, 1 direct, 2 idx8, 3 idx16)
static int s_fmt_cnt[8][26];     // per-fmt comp counts (POS: 2/3, TEX: 1/2)
static int s_fmt_frac[8][26];    // fixed-point fraction bits per fmt/attr
static int s_fmt_type[8][26];    // GXCompType (or color format for CLR attrs)
static int s_cur_fmt;
static const void* s_array_base[26];
static int s_array_stride[26];
static int s_zmode_enable = 0, s_zmode_write = 0;
static int s_atest_comp = 7, s_atest_ref = 0; // real GX alpha comparator (GXSetAlphaCompare)
static int s_cull_mode = 2; // GXCullMode: NONE=0 FRONT=1 BACK=2 ALL=3 (GX default BACK)
static int s_blend = 0x40 | (4 << 3) | 5; // blendcode: default SRCALPHA/INVSRCALPHA (see GXSetBlendMode)
static int s_atest = 0; // GX alpha compare active (discard alpha < ~0.5); set by GXSetAlphaCompare
// Texture matrices: GX_TEXMTX0..9 = ids 30,33..57; GX_IDENTITY = 60.
// P2D picture panes select their atlas sub-rect through these.
static float s_texmtx[10][2][4];

// Draw segments: contiguous runs of s_cap.out sharing texture + depth state +
// TEV program. The program is the RECORDED GX TEV state (see pc_tev_prog.h);
// the fragment shader evaluates it exactly — nothing is pre-solved here.
// alit/lmask/amba/mata/la0/la1: the GX alpha-channel lighting INPUTS at draw
// time (GX_ALPHA0 enable, light mask, ambient/material/light alphas). The bake
// does NOT light alpha (rasa = raw mat/vtx alpha) — these fields exist so a seg
// dump can show what a lit RASA would have been (milky-glass diagnosis).
static struct { int tex, start, count, depth, matsrc, alit; u32 lmask; float amba, mata, la0, la1; u32 progserial; PcTevProg prog; } s_segs[PC_CAP_MAX_SEGS];
static int s_nsegs;
// Segments the frame WANTED to open, uncapped — see the overflow path below.
static int s_nsegs_want;

// ---- full TEV state, tracked per GX contract (GXEnum.h verified) ----------
// One record per stage; every GXSetTev* setter writes here and marks the
// snapshot dirty. Nothing is interpreted until a triangle is pushed.
struct PcTevStage {
	u8 ca, cb, cc, cd;                    // GXTevColorArg
	u8 aa, ab, ac, ad;                    // GXTevAlphaArg
	u8 cop, cbias, cscale, cclamp, cout;  // GXSetTevColorOp
	u8 aop, abias, ascale, aclamp, aout;  // GXSetTevAlphaOp
	u8 texmap, texcoord, chan;            // GXSetTevOrder (0xFF = null)
	u8 kcsel, kasel;                      // GXSetTevKColorSel/KAlphaSel
	u8 rasswap, texswap;                  // GXSetTevSwapMode table ids (0..3)
};
// GXSetTevSwapModeTable: 4 tables x (r,g,b,a) -> source channel.
// GX resets all four to identity; Pikmin loads {R,R,R,A} into tables 1-3
// to broadcast RED for its desaturate/fade and damage-flash passes.
static u8 s_swaptbl[4][4] = { { 0, 1, 2, 3 }, { 0, 1, 2, 3 }, { 0, 1, 2, 3 }, { 0, 1, 2, 3 } };
static PcTevStage s_tev[PC_TEV_MAXSTAGES]; // initialized below (PC_TEVST0/N defaults)
static int s_ntevstages = 1;
static int s_tev_dirty = 1;     // any TEV-affecting state changed since last snapshot
static PcTevProg s_tevprog;     // current snapshot (rebuilt lazily when dirty)
static int s_need_sphere;       // an active stage samples a NRM-sourced texcoord
static int s_texmap[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
// Texcoord generators: source + matrix per GX_TEXCOORDn (GXSetTexCoordGen2).
static int s_tcg_src[8] = { 4, 4, 4, 4, 4, 4, 4, 4 }; // GX_TG_TEX0
static int s_tcg_mtx[8] = { 60, 60, 60, 60, 60, 60, 60, 60 }; // GX_IDENTITY
// TEV register file (C0..C2) + konst colors (K0..K3), snapshot per program.
// NOTE: S10 registers clamp to [0,1] here — Pikmin's observed shapes stay in
// range; signed/overrange TEV arithmetic is not modeled.
static float s_tev_regc[3][4] = { { 1, 1, 1, 1 }, { 1, 1, 1, 1 }, { 1, 1, 1, 1 } };
static float s_tev_konstc[4][4] = { { 1, 1, 1, 1 }, { 1, 1, 1, 1 }, { 1, 1, 1, 1 }, { 1, 1, 1, 1 } };
// Stage defaults per GX reset state: stage 0 = modulate (ZERO,TEXC,RASC,ZERO /
// ZERO,TEXA,RASA,ZERO), ADD->PREV, TEXMAP0/TEXCOORD0/COLOR0A0; stages 1+ pass
// PREV with no texture/channel. Field order matches PcTevStage.
#define PC_TEVST0 { 15, 8, 10, 15, 7, 4, 5, 7, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 4, 12, 28 }
#define PC_TEVSTN { 15, 15, 15, 0, 7, 7, 7, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0xFF, 0xFF, 0xFF, 12, 28 }
static const PcTevStage s_tev_defaults[PC_TEV_MAXSTAGES] = { PC_TEVST0, PC_TEVSTN, PC_TEVSTN, PC_TEVSTN,
	                                                         PC_TEVSTN, PC_TEVSTN, PC_TEVSTN, PC_TEVSTN };
static struct PcTevBoot {
	PcTevBoot() { memcpy(s_tev, s_tev_defaults, sizeof(s_tev)); }
} s_tev_boot;

// Lighting v1 state (functions live in the channel/light section below).
// Lights arrive in VIEW space (dgxGraphics::setLight pre-multiplies by the
// camera lookAt), matching the view-space position emit computes.
struct PCLightObj {
	u32 magic;
	u32 pad[2];
	float r, g, b, a;
	float aattn[3], kattn[3];
	float pos[3], dir[3];
};
#define PC_LIGHT_MAGIC 0x50434C54u // 'PCLT'
static PCLightObj s_lights[8];
static u32 s_chan0_mask;
// GXSetChanCtrl matSrc, tracked SEPARATELY for colour and alpha as GX does:
// PVW materials set GX_COLOR0 and GX_ALPHA0 with INDEPENDENT sources
// (dgxGraphics setLighting, MatSrcColor0Vtx / MatSrcAlpha0Vtx), e.g. a
// vertex-tinted surface with a material-wide fade. Collapsing them into
// one flag (and ignoring GX_ALPHA0 entirely) forced RGB and alpha to the
// same source — which is how the helmet's shine pass ended up with a
// WHITE RASC where GC has the mesh's dark vertex tint.
// GX_SRC_REG(0) / GX_SRC_VTX(1, default).
static int s_chan0_matsrc = 1;   // alpha source (name kept: existing refs)
static int s_chan0_matsrc_c = 1; // colour source
// GX_ALPHA0 lighting enable (GXSetChanCtrl). PVW materials enable it
// independently of the colour channel (EnableAlpha0); currently TRACKED ONLY
// (seg dump) — the bake still outputs unlit alpha.
static int s_chan0_alpha_lit;
extern float pc_gx_chan_mat[4]; // defined in the channel/light section below
int pc_gx_chan0_lit; // channel 0 lighting enable
float pc_gx_chan_amb[4] = { 0, 0, 0, 1 };
// Channel 1 (GX_COLOR1A1): the SPECULAR channel. Materials with EnableSpecular
// light it from slot 7 only (GX_AF_SPEC, dgxGraphics setLighting); TEV stages
// select it per stage via GXSetTevOrder's channel arg and typically ADD it on
// top of the diffuse pass — the "subtle shine".
static u32 s_chan1_mask;
int pc_gx_chan1_lit;
float pc_gx_chan1_mat[4] = { 1, 1, 1, 1 };
float pc_gx_chan1_amb[4] = { 0, 0, 0, 1 };

static void pc_cap_emit_vertex(void);

static void pc_cap_scalar(float v, int isInt) {
	if (!s_cap.active) return;
	int a = s_cap.cursor;
	if (a >= s_cap.nattrs) return;
	int attr = s_cap.attr[a];
	// fixed-point: integer-typed DIRECT pos/nrm/tex scalars carry frac bits
	if (isInt && s_vtxdesc[attr] == 1 && (attr == 9 || attr == 10 || (attr >= 13 && attr <= 20))) {
		int frac = s_fmt_frac[s_cur_fmt][attr];
		if (frac) v = v / (float)(1 << frac);
	}
	s_cap.scal[s_cap.filled++] = v;
	if (s_cap.filled >= s_cap.scalars[a]) {
		// Indexed attribute: scal[0] is the index into GXSetArray data.
		// Array data is NATIVE-endian: every GXSetArray source (mVertexList,
		// mNormalList, texcoords, colors, particle buffers) is a runtime
		// structure parsed via the byte-swapping stream readers — only DL
		// bytes themselves are big-endian.
		if (s_vtxdesc[attr] >= 2 && s_array_base[attr]) {
			int idx = (int)s_cap.scal[0];
			if (idx < 0 || idx > 0xFFFF) { s_cap.cursor = 0; s_cap.filled = 0; s_cap.active = 0; return; }
			const u8* p = (const u8*)s_array_base[attr] + (size_t)idx * s_array_stride[attr];
			int fattr = (attr == 25) ? 10 : attr; // NBT shares GX_VA_NRM's format
			int t = s_fmt_type[s_cur_fmt][fattr];
			int cnt = 0;
			if (attr == 9) cnt = (s_fmt_cnt[s_cur_fmt][attr] == 0) ? 2 : 3;
			else if (attr == 10 || attr == 25) cnt = 3; // NRM / NBT: fetch the normal
			else if (attr >= 13 && attr <= 20) cnt = (s_fmt_cnt[s_cur_fmt][attr] == 0) ? 1 : 2;
			if (attr == 11 || attr == 12) {
				// color array: GX_RGBA8 (4B) or GX_RGB8 (3B)
				s_cap.clr[0] = p[0] / 255.0f;
				s_cap.clr[1] = p[1] / 255.0f;
				s_cap.clr[2] = p[2] / 255.0f;
				s_cap.clr[3] = (t == 5) ? p[3] / 255.0f : 1.0f;
			} else {
				float vals[3] = { 0, 0, 0 };
				int frac = s_fmt_frac[s_cur_fmt][fattr];
				for (int i = 0; i < cnt && i < 3; i++) {
					float fv = 0;
					if (t == 4) { // F32
						float raw;
						memcpy(&raw, p + i * 4, 4);
						fv = raw;
					} else if (t == 3) { // S16
						s16 raw;
						memcpy(&raw, p + i * 2, 2);
						fv = (float)raw;
					} else if (t == 2) { // U16
						u16 raw;
						memcpy(&raw, p + i * 2, 2);
						fv = (float)raw;
					} else if (t == 1) { // S8
						fv = (float)(s8)p[i];
					} else { // U8
						fv = (float)p[i];
					}
					if (frac && t != 4) fv = fv / (float)(1 << frac);
					vals[i] = fv;
				}
				if (attr == 9) {
					s_cap.pos[0] = vals[0];
					s_cap.pos[1] = vals[1];
					s_cap.pos[2] = (cnt > 2) ? vals[2] : 0.0f;
				} else if (attr == 10 || attr == 25) { // NRM / NBT: first 3 = normal
					s_cap.nrm[0] = vals[0];
					s_cap.nrm[1] = vals[1];
					s_cap.nrm[2] = vals[2];
				} else if (attr == 13) {
					s_cap.st[0] = vals[0];
					s_cap.st[1] = (cnt > 1) ? vals[1] : 0.0f;
				} else if (attr == 14) { // GX_VA_TEX1
					s_cap.st1[0] = vals[0];
					s_cap.st1[1] = (cnt > 1) ? vals[1] : 0.0f;
				}
			}
			s_cap.cursor++;
			s_cap.filled = 0;
			if (s_cap.cursor >= s_cap.nattrs) pc_cap_emit_vertex();
			return;
		}
		if (attr == 0) { // GX_VA_PNMTXIDX: per-vertex matrix palette select
			s_cap.vtx_mtx_slot = ((int)s_cap.scal[0]) / 3;
			if (s_cap.vtx_mtx_slot > pc_gx_vtxidx_maxslot) pc_gx_vtxidx_maxslot = s_cap.vtx_mtx_slot;
		} else if (attr == 9) { // GX_VA_POS
			s_cap.pos[0] = s_cap.scal[0];
			s_cap.pos[1] = s_cap.scal[1];
			s_cap.pos[2] = (s_cap.scalars[a] > 2) ? s_cap.scal[2] : 0.0f;
		} else if (attr == 10 || attr == 25) { // GX_VA_NRM/NBT: first 3 = normal
			s_cap.nrm[0] = s_cap.scal[0];
			s_cap.nrm[1] = s_cap.scal[1];
			s_cap.nrm[2] = s_cap.scal[2];
		} else if (attr == 11 || attr == 12) { // GX_VA_CLR0/1
			if (s_cap.scalars[a] == 4) {
				for (int i = 0; i < 4; i++) s_cap.clr[i] = s_cap.scal[i] / 255.0f;
			}
		} else if (attr == 13) { // GX_VA_TEX0
			s_cap.st[0] = s_cap.scal[0];
			s_cap.st[1] = (s_cap.scalars[a] > 1) ? s_cap.scal[1] : 0.0f;
		} else if (attr == 14) { // GX_VA_TEX1
			s_cap.st1[0] = s_cap.scal[0];
			s_cap.st1[1] = (s_cap.scalars[a] > 1) ? s_cap.scal[1] : 0.0f;
		}
		s_cap.cursor++;
		s_cap.filled = 0;
		if (s_cap.cursor >= s_cap.nattrs) pc_cap_emit_vertex();
	}
}

extern "C" void pc_gx_fifo_u8(u8 v) { pc_cap_scalar((float)v, 1); }
extern "C" void pc_gx_fifo_s8(s8 v) { pc_cap_scalar((float)v, 1); }
extern "C" void pc_gx_fifo_u16(u16 v) { pc_cap_scalar((float)v, 1); }
extern "C" void pc_gx_fifo_s16(s16 v) { pc_cap_scalar((float)v, 1); }
extern "C" void pc_gx_fifo_f32(f32 v) { pc_cap_scalar(v, 0); }
extern "C" void pc_gx_fifo_s32(s32 v) { pc_cap_scalar((float)v, 1); }
extern "C" void pc_gx_fifo_u32(u32 v) {
	// A u32 write is a whole RGBA color (GXColor1u32) when the current
	// attribute is a color; otherwise treat as one scalar.
	if (s_cap.active && s_cap.cursor < s_cap.nattrs) {
		int attr = s_cap.attr[s_cap.cursor];
		if (attr == 11 || attr == 12) {
			s_cap.clr[0] = ((v >> 24) & 0xFF) / 255.0f;
			s_cap.clr[1] = ((v >> 16) & 0xFF) / 255.0f;
			s_cap.clr[2] = ((v >> 8) & 0xFF) / 255.0f;
			s_cap.clr[3] = (v & 0xFF) / 255.0f;
			s_cap.cursor++;
			s_cap.filled = 0;
			if (s_cap.cursor >= s_cap.nattrs) pc_cap_emit_vertex();
			return;
		}
	}
	pc_cap_scalar((float)v, 0);
}

// Rebuild the current TEV program snapshot from the tracked state (lazy: only
// when a TEV-affecting setter ran since the last build). Segments break when
// the program CONTENT changes, so back-to-back materials with identical TEV
// still batch.
static u32 s_tevprog_serial;
static void pc_tev_snapshot(void) {
	if (!s_tev_dirty) return;
	s_tev_dirty = 0;
	static PcTevProg tmp;
	PcTevProg* P = &tmp;
	memset(P, 0, sizeof(*P));
	int n = s_ntevstages < 1 ? 1 : (s_ntevstages > PC_TEV_MAXSTAGES ? PC_TEV_MAXSTAGES : s_ntevstages);
	P->nstages = n;
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 4; c++) P->regs[r + 1][c] = s_tev_regc[r][c]; // regs[0] = PREV = 0
	for (int k = 0; k < 4; k++)
		for (int c = 0; c < 4; c++) P->konst[k][c] = s_tev_konstc[k][c];
	for (int t = 0; t < 4; t++)
		for (int c = 0; c < 4; c++) P->swaptbl[t][c] = s_swaptbl[t][c];
	int sphere = 0;
	for (int i = 0; i < n; i++) {
		const PcTevStage* S = &s_tev[i];
		P->cargs[i][0] = S->ca; P->cargs[i][1] = S->cb; P->cargs[i][2] = S->cc; P->cargs[i][3] = S->cd;
		P->aargs[i][0] = S->aa; P->aargs[i][1] = S->ab; P->aargs[i][2] = S->ac; P->aargs[i][3] = S->ad;
		P->cops[i][0] = S->cop; P->cops[i][1] = S->cbias; P->cops[i][2] = S->cscale; P->cops[i][3] = S->cout & 3;
		P->aops[i][0] = S->aop; P->aops[i][1] = S->abias; P->aops[i][2] = S->ascale; P->aops[i][3] = S->aout & 3;
		P->swap[i][0] = S->rasswap; P->swap[i][1] = S->texswap; P->swap[i][2] = 0; P->swap[i][3] = 0;
		int texflag = PC_TEVTEX_NONE, texh = -1;
		if (S->texmap < 8) {
			int h = s_texmap[S->texmap];
			// -1 (nothing bound) / -2 (unpopulated EFB grab) / -3: no texture on
			// our side for a configured map -> neutral white texel, so a multiply
			// stage passes through instead of blacking out.
			if (h >= 0) { texflag = PC_TEVTEX_SAMPLE; texh = h; }
			else texflag = PC_TEVTEX_WHITE;
		}
		int coord = (S->texcoord < 8) ? S->texcoord : 0;
		if (texflag != PC_TEVTEX_NONE && s_tcg_src[coord] == 1 /*GX_TG_NRM*/) sphere = 1;
		// Channel select: 0 = none (NULL/ZERO -> ras 0), 1 = COLOR0A0 (diffuse),
		// 2 = COLOR1A1 (the specular channel — GX_COLOR1=1/ALPHA1=3/COLOR1A1=5).
		int chansel;
		if (S->chan == 0xFF || S->chan == 6 /*GX_COLOR_ZERO*/) chansel = 0;
		else if (S->chan == 1 || S->chan == 3 || S->chan == 5) chansel = 2;
		else chansel = 1;
		P->tex[i][0] = texflag;
		P->tex[i][1] = (coord == 1) ? 1 : 0; // uvsel: vertex carries gen'd coord0 + coord1
		P->tex[i][2] = chansel;
		P->tex[i][3] = S->kcsel;
		P->misc[i][0] = S->cclamp ? 1 : 0;
		P->misc[i][1] = S->aclamp ? 1 : 0;
		P->misc[i][2] = S->kasel;
		P->misc[i][3] = texh;
	}
	P->lastco = s_tev[n - 1].cout & 3;
	P->lastao = s_tev[n - 1].aout & 3;
	P->atest = (s_atest_comp & 7) | (s_atest_ref << 3);
	if (memcmp(&s_tevprog, P, sizeof(PcTevProg)) != 0) {
		memcpy(&s_tevprog, P, sizeof(PcTevProg));
		s_tevprog_serial++;
		s_need_sphere = sphere;
		if (getenv("PC_TEV_TRACE")) {
			static u32 seen[512]; static int nseen;
			u32 hash = 2166136261u;
			const u8* q = (const u8*)P;
			for (size_t j = 0; j < sizeof(PcTevProg); j++) { hash ^= q[j]; hash *= 16777619u; }
			bool found = false;
			for (int j = 0; j < nseen; j++) if (seen[j] == hash) { found = true; break; }
			if (!found && nseen < 512) {
				seen[nseen++] = hash;
				fprintf(stderr, "[TEV] nst=%d last=c%d/a%d", n, P->lastco, P->lastao);
				for (int i = 0; i < n; i++)
					fprintf(stderr, " | s%d c(%d,%d,%d,%d)op(%d,%d,%d>%d) a(%d,%d,%d,%d)op(%d,%d,%d>%d) tex=%d/%d uv%d ras%d k%d/%d",
					        i, P->cargs[i][0], P->cargs[i][1], P->cargs[i][2], P->cargs[i][3],
					        P->cops[i][0], P->cops[i][1], P->cops[i][2], P->cops[i][3],
					        P->aargs[i][0], P->aargs[i][1], P->aargs[i][2], P->aargs[i][3],
					        P->aops[i][0], P->aops[i][1], P->aops[i][2], P->aops[i][3],
					        P->tex[i][0], P->misc[i][3], P->tex[i][1], P->tex[i][2], P->tex[i][3], P->misc[i][2]);
				fprintf(stderr, "\n");
			}
		}
	}
}

static void pc_cap_push_tri(float* a, float* b, float* c) {
	s_cap.wantv += 3;
	if (s_cap.outv + 3 > PC_CAP_MAX_VERTS) {
		// Dropping geometry is a visible bug, not a benign clamp. Say so once,
		// rather than silently drawing a different scene than the game asked for.
		static int warned = 0;
		if (!warned) {
			warned = 1;
			fprintf(stderr, "[VTX] *** vertex capture FULL at %d verts - dropping triangles. "
			                "Geometry and UI will flicker. Raise PC_CAP_MAX_VERTS.\n", PC_CAP_MAX_VERTS);
		}
		return;
	}
	// open/extend the segment for the current texture + depth + TEV program
	int depth = (s_zmode_enable ? 1 : 0) | (s_zmode_write ? 2 : 0) | (s_blend << 2) | (s_atest << 9)
	          | ((s_cull_mode & 3) << 10);
	pc_tev_snapshot();
	if ((s_cap.curTex == -1 || s_cap.curTex == -3) && getenv("PC_TEXDIAG")) {
		static u32 seen[64]; static int nseen;
		int t0m = s_tev[0].texmap < 8 ? s_tev[0].texmap : -1;
		int t1m = s_tev[1].texmap < 8 ? s_tev[1].texmap : -1;
		int tc1nrm = (s_tcg_src[1] == 1) ? 1 : 0;
		u32 key = ((u32)(t0m & 0xf) << 20) | ((u32)(t1m & 0xf) << 16) | ((u32)(s_ntevstages & 0xf) << 12)
		        | ((u32)(tc1nrm & 1) << 8) | ((u32)(pc_gx_chan0_lit & 1) << 4) | (s_cap.has_nrm & 1);
		bool found = false;
		for (int i = 0; i < nseen; i++) if (seen[i] == key) { found = true; break; }
		if (!found && nseen < 64) {
			seen[nseen++] = key;
			fprintf(stderr, "[TEXDIAG] untextured lit=%d nrm=%d tev0map=%d tev1map=%d nstages=%d tc1nrm=%d map0h=%d map1h=%d matc=(%.2f,%.2f,%.2f)\n",
			        pc_gx_chan0_lit, s_cap.has_nrm, t0m, t1m, s_ntevstages, tc1nrm,
			        t0m >= 0 ? s_texmap[t0m] : -1, t1m >= 0 ? s_texmap[t1m] : -1,
			        s_cap.clr[0], s_cap.clr[1], s_cap.clr[2]);
		}
	}
	if (s_nsegs == 0 || s_segs[s_nsegs - 1].tex != s_cap.curTex || s_segs[s_nsegs - 1].depth != depth
	    || s_segs[s_nsegs - 1].progserial != s_tevprog_serial) {
		s_nsegs_want++;
		if (s_nsegs >= PC_CAP_MAX_SEGS) {
			// Loud, once. The vertex cap already learned this lesson: a silent
			// capacity clamp that changes what is drawn is a mystery visual bug
			// for whoever hits it next.
			static int warned = 0;
			if (!warned) {
				warned = 1;
				fprintf(stderr, "[VTX] *** segment array FULL at %d - further triangles are merged into the "
				                "PREVIOUS segment and will draw with the WRONG texture. Raise PC_CAP_MAX_SEGS.\n",
				        PC_CAP_MAX_SEGS);
			}
		}
		if (s_nsegs < PC_CAP_MAX_SEGS) {
			s_segs[s_nsegs].tex = s_cap.curTex;
			s_segs[s_nsegs].start = s_cap.outv;
			s_segs[s_nsegs].count = 0;
			s_segs[s_nsegs].depth = depth;
			// pack both sources for the dump: bit0 = alpha src, bit1 = colour src
			s_segs[s_nsegs].matsrc = s_chan0_matsrc | (s_chan0_matsrc_c << 1);
			s_segs[s_nsegs].alit = s_chan0_alpha_lit;
			s_segs[s_nsegs].lmask = s_chan0_mask;
			s_segs[s_nsegs].amba = pc_gx_chan_amb[3];
			s_segs[s_nsegs].mata = pc_gx_chan_mat[3];
			s_segs[s_nsegs].la0 = s_lights[0].magic == PC_LIGHT_MAGIC ? s_lights[0].a : -1.0f;
			s_segs[s_nsegs].la1 = s_lights[1].magic == PC_LIGHT_MAGIC ? s_lights[1].a : -1.0f;
			s_segs[s_nsegs].progserial = s_tevprog_serial;
			memcpy(&s_segs[s_nsegs].prog, &s_tevprog, sizeof(PcTevProg));
			s_nsegs++;
		}
	}
	if (s_nsegs) s_segs[s_nsegs - 1].count += 3;
	float* d = &s_cap.out[s_cap.outv * 16];
	float* src[3] = { a, b, c };
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 16; j++) d[i * 16 + j] = src[i][j];
	s_cap.outv += 3;
}

// ---- line / point primitives --------------------------------------------
// This renderer submits triangles only, so GX_LINES/LINESTRIP/POINTS are
// widened into screen-space quads. Needed for real content: the day-results
// screen graphs Pikmin counts with GX_LINESTRIP at a 3px width
// (ogGraph.cpp:272-273) — before this, those vertices fell through the
// primitive assembler, quadn ran past quad[4] and corrupted the capture
// struct (user crash dumps/crash_on_save).
extern float pc_gx_line_width;
extern float pc_gx_point_size;

// Emit a quad from four clip-space corners given as NDC offsets applied to
// two endpoints. Width is converted from pixels to NDC using the 640x480
// logical framebuffer the game renders through, then scaled back into clip
// space by each vertex's own w so it survives the perspective divide.
static void pc_cap_push_wide(float* a, float* b, float halfPx) {
	float wa = a[3], wb = b[3];
	if (wa == 0.0f || wb == 0.0f) return;
	float ax = a[0] / wa, ay = a[1] / wa;
	float bx = b[0] / wb, by = b[1] / wb;
	// Work in PIXELS so thickness is uniform at any angle — a graph is mostly
	// diagonals, and doing this in NDC (whose axes have different pixel
	// scales) would thin or fatten them with slope. NDC spans 2.0 across the
	// 640x480 logical framebuffer, so half-extents are 320 and 240.
	float dx = (bx - ax) * 320.0f, dy = (by - ay) * 240.0f;
	float len = sqrtf(dx * dx + dy * dy);
	if (len < 1e-6f) { dx = 1.0f; dy = 0.0f; len = 1.0f; }
	dx /= len;
	dy /= len;
	float px = (-dy * halfPx) / 320.0f; // perpendicular, half-width, back to NDC
	float py = (dx * halfPx) / 240.0f;

	float v0[16], v1[16], v2[16], v3[16];
	for (int j = 0; j < 16; j++) { v0[j] = v1[j] = a[j]; v2[j] = v3[j] = b[j]; }
	v0[0] = (ax + px) * wa; v0[1] = (ay + py) * wa;
	v1[0] = (ax - px) * wa; v1[1] = (ay - py) * wa;
	v2[0] = (bx - px) * wb; v2[1] = (by - py) * wb;
	v3[0] = (bx + px) * wb; v3[1] = (by + py) * wb;
	pc_cap_push_tri(v0, v1, v2);
	pc_cap_push_tri(v0, v2, v3);
}

static void pc_cap_push_line(float* a, float* b) {
	float half = pc_gx_line_width * 0.5f;
	if (half < 0.5f) half = 0.5f;
	if (getenv("PC_VERBOSE")) {
		static int n;
		if (n < 4) {
			n++;
			fprintf(stderr, "[LINE] widened segment %d (width %.1fpx)\n", n, pc_gx_line_width);
		}
	}
	pc_cap_push_wide(a, b, half);
}

static void pc_cap_push_point(float* a) {
	float half = pc_gx_point_size * 0.5f;
	if (half < 0.5f) half = 0.5f;
	// nudge a second point one half-width to the right so the degenerate
	// "line" widens into a square centred on the point
	float b[16];
	for (int j = 0; j < 16; j++) b[j] = a[j];
	float w = a[3];
	if (w == 0.0f) return;
	b[0] = (a[0] / w + half / 320.0f) * w;
	pc_cap_push_wide(a, b, half);
}

// Generate GX_TEXCOORDn for the current vertex: source attr (TEX0/TEX1) or a
// sphere-map of the view-space normal (GX_TG_NRM), then the coord's texture
// matrix (P2D picture panes select their atlas sub-rect through these).
static void pc_gen_texcoord(int coord, const float* n, float* out) {
	int src = s_tcg_src[coord & 7];
	if (src == 1) { // GX_TG_NRM: sphere-map approximation from the view normal
		// The Y term is NEGATED where X is not. That asymmetry mirrors the
		// environment map VERTICALLY, which is the leading explanation for the
		// user-reported lighting-direction error (v59): the affected surfaces
		// (ship windshield, green light domes, Olimar's helmet) are env-mapped,
		// so their "light direction" is a texture lookup on the normal — which
		// explains why the error magnitude VARIES per surface instead of being a
		// clean 180, why PC_NO_SPEC barely changed it, and why PC_LIGHT_FLIP had
		// NO effect at all (no light is involved).
		// FIXED + USER-VERIFIED (v60): the un-negated Y is correct. It "fixes a
		// bunch of shine and shading on reflective surfaces", and even on
		// Olimar's helmet the shine SPOT now matches Dolphin's position (the
		// helmet's remaining milkiness is the separate seg-94 issue).
		// PC_SPHERE_NEGY=1 restores the old mirrored form for A/B.
		static int s_sph_negy = -1;
		if (s_sph_negy < 0) s_sph_negy = getenv("PC_SPHERE_NEGY") ? 1 : 0;
		out[0] = n[0] * 0.5f + 0.5f;
		out[1] = s_sph_negy ? (0.5f - n[1] * 0.5f) : (n[1] * 0.5f + 0.5f);
		return;
	}
	const float* st = (src == 5 /*GX_TG_TEX1*/) ? s_cap.st1 : s_cap.st;
	float ts = st[0], tt = st[1];
	int mtx = s_tcg_mtx[coord & 7];
	if (mtx >= 30 && mtx <= 57) {
		const float(*T)[4] = s_texmtx[(mtx - 30) / 3];
		float s2 = T[0][0] * ts + T[0][1] * tt + T[0][3];
		float t2 = T[1][0] * ts + T[1][1] * tt + T[1][3];
		ts = s2;
		tt = t2;
		if (pc_gx_in_dl && coord == 0) pc_gx_dl_st_texmtx_applied++;
	}
	out[0] = ts;
	out[1] = tt;
}

static void pc_cap_emit_vertex(void) {
	pc_tev_snapshot(); // s_need_sphere must reflect the current program
	// model -> view via 3x4 posmtx, -> clip via 4x4 projection
	float m[3];
	int slot = (s_cap.vtx_mtx_slot >= 0) ? s_cap.vtx_mtx_slot : s_cap.cur_mtx_slot;
	if (slot < 0 || slot >= 16) slot = 0;
	const float(*M)[4] = s_cap.mtxpal[slot];
	for (int i = 0; i < 3; i++)
		m[i] = M[i][0] * s_cap.pos[0] + M[i][1] * s_cap.pos[1] + M[i][2] * s_cap.pos[2] + M[i][3];
	float clip[4];
	for (int i = 0; i < 4; i++)
		clip[i] = s_cap.proj[i][0] * m[0] + s_cap.proj[i][1] * m[1] + s_cap.proj[i][2] * m[2] + s_cap.proj[i][3];
	float* v = s_cap.quad[s_cap.quadn];
	v[0] = clip[0]; v[1] = clip[1]; v[2] = clip[2]; v[3] = clip[3];
	// GX channel lighting: rasc = matColor * clamp(amb + sum(diffuse)).
	// s_cap.clr already holds the mat color (or the vertex color).
	float illum[3] = { 1, 1, 1 };
	float n[3] = { 0, 0, 1 };
	if ((pc_gx_chan0_lit || s_chan0_alpha_lit || pc_gx_chan1_lit || s_need_sphere) && s_cap.has_nrm) {
		const float(*NM)[4] = s_cap.nrmpal[slot];
		for (int i = 0; i < 3; i++)
			n[i] = NM[i][0] * s_cap.nrm[0] + NM[i][1] * s_cap.nrm[1] + NM[i][2] * s_cap.nrm[2];
		float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		if (len > 1e-6f) { n[0] /= len; n[1] /= len; n[2] /= len; }
		// PC_NRM_FLIP=1: negate the transformed normal. A/B probe for the
		// user-reported inverted highlight direction (v55). PC_NO_SPEC showed
		// the inversion survives with specular off, so it is channel-0/diffuse
		// or its normals. If flipping here makes ship highlights MATCH Dolphin,
		// the normals (or the normal matrix) carry a sign error. If it merely
		// swaps which side is wrong without matching, the fault is the SPACE the
		// light positions live in (our view matrix vs the game's), not the
		// normals.
		static int s_nrm_flip = -1;
		if (s_nrm_flip < 0) s_nrm_flip = getenv("PC_NRM_FLIP") ? 1 : 0;
		if (s_nrm_flip) { n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; }
	}
	// GX lights the ALPHA channel independently of colour (GX_ALPHA0 enable):
	// rasa = mat_a * clamp(amb_a + sum(N.L * light_a)). PVW glass materials
	// rely on this — the helmet shine pass is authored mat_a=1.0 with ambient
	// alpha ~0.19, so on GC most of the dome resolves nearly transparent and
	// only light-facing regions approach 1.0. Baking raw mat_a=1.0 instead
	// turned the pass's broad TEXA mask into a uniform ~32% white veil (the
	// "milky vs translucent" helmet). PC_NO_LIT_ALPHA=1 reverts.
	static int s_no_lit_alpha = -1;
	if (s_no_lit_alpha < 0) s_no_lit_alpha = getenv("PC_NO_LIT_ALPHA") ? 1 : 0;
	bool litA = s_chan0_alpha_lit && !s_no_lit_alpha && s_cap.has_nrm;
	float illum_a = 1.0f;
	if ((pc_gx_chan0_lit || litA) && s_cap.has_nrm) {
		float acc_a = pc_gx_chan_amb[3];
		// COLOUR illum starts at ambient only when the colour channel is lit;
		// an alpha-only-lit material (helmet shine) keeps illum {1,1,1} and the
		// clamp below returns it to 1.0 after the (unused) accumulation.
		if (pc_gx_chan0_lit) {
			illum[0] = pc_gx_chan_amb[0]; illum[1] = pc_gx_chan_amb[1]; illum[2] = pc_gx_chan_amb[2];
		}
		for (int li = 0; li < 7; li++) {
			const PCLightObj* L = &s_lights[li];
			if (!(s_chan0_mask & (1u << li)) || L->magic != PC_LIGHT_MAGIC) continue;
			// PC_LIGHT_FLIP=x|y|z (v58): negate one axis of the light position
			// before computing the light vector. Probe for the user-reported
			// lighting direction error whose MAGNITUDE VARIES per surface (180
			// deg on the ship's side protrusions, less on the windshield) —
			// varying magnitude rules out a normal-sign flip (that would be a
			// uniform 180) and points at a MIRRORED AXIS in light space. If one
			// of x/y/z makes the ship match Dolphin, that axis' sign is wrong
			// where light positions enter view space.
			static int s_lflip = -2;
			if (s_lflip == -2) {
				const char* e = getenv("PC_LIGHT_FLIP");
				s_lflip = !e ? -1 : (*e == 'x' ? 0 : *e == 'y' ? 1 : *e == 'z' ? 2 : -1);
			}
			float lp[3] = { L->pos[0], L->pos[1], L->pos[2] };
			if (s_lflip >= 0) lp[s_lflip] = -lp[s_lflip];
			float d[3] = { lp[0] - m[0], lp[1] - m[1], lp[2] - m[2] };
			float dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
			if (dl > 1e-6f) { d[0] /= dl; d[1] /= dl; d[2] /= dl; }
			float ndl = n[0] * d[0] + n[1] * d[1] + n[2] * d[2];
			if (ndl <= 0.0f) continue;
			// GX_AF_SPOT attenuation — dgxGraphics passes it for EVERY lit
			// channel (setLighting), and this loop previously ignored it, so
			// every light reached every vertex at full strength ("lit side
			// runs strong", user-verified everywhere). Per GX/Dolphin:
			//   t    = max(0, dot(dir_vertex_to_light, light.dir))
			//   attn = max(0, a0 + a1*t + a2*t*t) / (k0 + k1*d + k2*d*d)
			// Pikmin's point lights author neutral cones (a=1,0,0; dir left
			// zero) but REAL distance falloff via setLightDistAttn. An
			// all-zero aattn/kattn means the light never saw GXInitLightAttn
			// (memset init) — treat each as neutral rather than killing the
			// light. PC_NO_ATTN=1 reverts.
			static int s_no_attn = -1;
			if (s_no_attn < 0) s_no_attn = getenv("PC_NO_ATTN") ? 1 : 0;
			float attn = 1.0f;
			if (!s_no_attn) {
				// SDK GXInitLightDir STORES the NEGATED direction
				// (GXLight.c:275) and hardware dots vertex->light against the
				// stored value; our shim stores the raw facing, so negate here
				// (on-axis vertex => t=+1, SPOT_Smooth ang=1 at center, 0 at
				// the cone edge).
				float t = -(d[0] * L->dir[0] + d[1] * L->dir[1] + d[2] * L->dir[2]);
				if (t < 0.0f) t = 0.0f;
				float ang = L->aattn[0] + L->aattn[1] * t + L->aattn[2] * t * t;
				if (ang < 0.0f) ang = 0.0f;
				if (L->aattn[0] == 0.0f && L->aattn[1] == 0.0f && L->aattn[2] == 0.0f) ang = 1.0f;
				float div = L->kattn[0] + L->kattn[1] * dl + L->kattn[2] * dl * dl;
				attn = (div > 1e-6f) ? ang / div : ang;
			}
			illum[0] += attn * ndl * L->r; illum[1] += attn * ndl * L->g; illum[2] += attn * ndl * L->b;
			acc_a += attn * ndl * L->a;
		}
		for (int i = 0; i < 3; i++) illum[i] = illum[i] < 0 ? 0 : (illum[i] > 1 ? 1 : illum[i]);
		if (litA) illum_a = acc_a < 0 ? 0 : (acc_a > 1 ? 1 : acc_a);
	}
	// Bake the honest per-vertex inputs; the fragment shader evaluates the
	// recorded TEV program against them. v[4..7] = RAS (the lit rasterized
	// channel color), v[8..9] / v[10..11] = generated GX_TEXCOORD0/1.
	// When the channel sources its material from the register (GX_SRC_REG), the
	// rasterized alpha comes from GXSetChanMatColor (a real fade, e.g. the dim
	// overlay's mAlpha) — not the white vertex color. GXSetTevOp(GX_PASSCLR)
	// resets matsrc→VTX so flat PASSCLR draws that merely INHERITED a stale REG
	// (the glass shine/frame) keep their own vertex alpha instead of being
	// forced opaque. RGB NOW ALSO honours the register (v45): the old
	// shortcut ("register RGB is white there anyway") is false for materials
	// that TINT through the channel register. Olimar's helmet shine pass is
	// matsrc=REG with TEV out = TEXC + RASC*TEXA; taking RASC from the white
	// VERTEX colour made that TEXC+TEXA -> clamped WHITE BLOB over his face,
	// where GC uses the register's dim/tinted RGB and stays see-through.
	// The register defaults to white, so this is a no-op for every material
	// that never sets it. PC_NO_MATREG_RGB=1 restores the old behaviour.
	extern float pc_gx_chan_mat[4];
	static int s_no_matreg_rgb = -1;
	if (s_no_matreg_rgb < 0) s_no_matreg_rgb = getenv("PC_NO_MATREG_RGB") ? 1 : 0;
	bool useReg = (s_chan0_matsrc_c == 0) && !s_no_matreg_rgb;
	float rasa = (s_chan0_matsrc == 0) ? pc_gx_chan_mat[3] : s_cap.clr[3];
	v[4] = (useReg ? pc_gx_chan_mat[0] : s_cap.clr[0]) * illum[0];
	v[5] = (useReg ? pc_gx_chan_mat[1] : s_cap.clr[1]) * illum[1];
	v[6] = (useReg ? pc_gx_chan_mat[2] : s_cap.clr[2]) * illum[2];
	v[7] = rasa * illum_a;
	float uv0[2], uv1[2];
	pc_gen_texcoord(0, n, uv0);
	pc_gen_texcoord(1, n, uv1);
	if (pc_gx_in_dl) {
		if (uv0[0] < pc_gx_dl_st_min[0]) pc_gx_dl_st_min[0] = uv0[0];
		if (uv0[0] > pc_gx_dl_st_max[0]) pc_gx_dl_st_max[0] = uv0[0];
		if (uv0[1] < pc_gx_dl_st_min[1]) pc_gx_dl_st_min[1] = uv0[1];
		if (uv0[1] > pc_gx_dl_st_max[1]) pc_gx_dl_st_max[1] = uv0[1];
	}
	v[8] = uv0[0]; v[9] = uv0[1];
	v[10] = uv1[0]; v[11] = uv1[1];
	// v[12..15] = channel 1 (GX_COLOR1A1, specular): mat1 * clamp(amb1 + spec
	// lighting). GX hardware attenuation for GX_AF_SPEC: d = N·half (half-angle
	// stored by GXInitSpecularDir), atten = max(0, a0+a1·d+a2·d²)/(k0+k1·d+
	// k2·d²), gated on the normal facing the light. A DISABLED channel outputs
	// its material color unlit (GX semantics).
	{
		float spec[3] = { pc_gx_chan1_mat[0], pc_gx_chan1_mat[1], pc_gx_chan1_mat[2] };
		if (pc_gx_chan1_lit && s_cap.has_nrm) {
			float acc[3] = { pc_gx_chan1_amb[0], pc_gx_chan1_amb[1], pc_gx_chan1_amb[2] };
			for (int li = 0; li < 8; li++) {
				const PCLightObj* L = &s_lights[li];
				if (!(s_chan1_mask & (1u << li)) || L->magic != PC_LIGHT_MAGIC) continue;
				float g[3] = { L->pos[0], L->pos[1], L->pos[2] };
				if (!L->pad[0]) { g[0] -= m[0]; g[1] -= m[1]; g[2] -= m[2]; }
				float gl = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
				if (gl > 1e-6f) { g[0] /= gl; g[1] /= gl; g[2] /= gl; }
				if (n[0] * g[0] + n[1] * g[1] + n[2] * g[2] <= 0.0f) continue; // facing away
				float d = n[0] * L->dir[0] + n[1] * L->dir[1] + n[2] * L->dir[2];
				if (d < 0.0f) d = 0.0f;
				float num = L->aattn[0] + L->aattn[1] * d + L->aattn[2] * d * d;
				float den = L->kattn[0] + L->kattn[1] * d + L->kattn[2] * d * d;
				float att = (den > 1e-6f && num > 0.0f) ? num / den : 0.0f;
				acc[0] += att * L->r; acc[1] += att * L->g; acc[2] += att * L->b;
			}
			for (int i = 0; i < 3; i++) {
				float a = acc[i] < 0 ? 0 : (acc[i] > 1 ? 1 : acc[i]);
				spec[i] = pc_gx_chan1_mat[i] * a;
			}
		}
		// PC_NO_SPEC=1: zero the specular channel — attribution toggle for
		// "white patch" reports (if the artifact vanishes, it's chan-1 spec).
		static int s_no_spec = -1;
		if (s_no_spec < 0) s_no_spec = getenv("PC_NO_SPEC") ? 1 : 0;
		if (s_no_spec) { spec[0] = spec[1] = spec[2] = 0.0f; }
		v[12] = spec[0]; v[13] = spec[1]; v[14] = spec[2];
		v[15] = pc_gx_chan1_mat[3]; // ALPHA1 is unlit here (enable=FALSE): mat alpha
	}
	s_cap.quadn++;
	s_cap.got++;
	s_cap.cursor = 0;
	s_cap.filled = 0;
	// Hard bound: quad[] holds 4 vertices and every case below resets quadn,
	// but an unhandled primitive would otherwise walk straight off the end of
	// the capture struct (GX_LINESTRIP did exactly that on the day-results
	// graph until lines were implemented below).
	if (s_cap.quadn >= 4 && s_cap.prim != 0x80) s_cap.quadn = 0;
	// prim types: 0x80 quads, 0x90 tris, 0x98 tristrip, 0xA0 trifan,
	//             0xA8 lines, 0xB0 linestrip, 0xB8 points
	if (s_cap.prim == 0xA8 || s_cap.prim == 0xB0) {
		if (s_cap.quadn == 2) {
			pc_cap_push_line(s_cap.quad[0], s_cap.quad[1]);
			if (s_cap.prim == 0xB0) { // strip: the endpoint starts the next segment
				for (int j = 0; j < 16; j++) s_cap.quad[0][j] = s_cap.quad[1][j];
				s_cap.quadn = 1;
			} else {
				s_cap.quadn = 0;
			}
		}
	} else if (s_cap.prim == 0xB8) {
		pc_cap_push_point(s_cap.quad[0]);
		s_cap.quadn = 0;
	} else if (s_cap.prim == 0x80 && s_cap.quadn == 4) {
		pc_cap_push_tri(s_cap.quad[0], s_cap.quad[1], s_cap.quad[2]);
		pc_cap_push_tri(s_cap.quad[0], s_cap.quad[2], s_cap.quad[3]);
		s_cap.quadn = 0;
	} else if (s_cap.prim == 0x90 && s_cap.quadn == 3) {
		pc_cap_push_tri(s_cap.quad[0], s_cap.quad[1], s_cap.quad[2]);
		s_cap.quadn = 0;
	} else if ((s_cap.prim == 0x98 || s_cap.prim == 0xA0) && s_cap.quadn == 3) {
		// Triangle strips ALTERNATE winding by definition; the hardware culls
		// with per-triangle parity. Expanding to independent triangles must
		// restore a consistent winding (swap the first two verts on odd
		// triangles) or backface culling eats every other strip triangle.
		if (s_cap.prim == 0x98 && (s_cap.strip_odd & 1))
			pc_cap_push_tri(s_cap.quad[1], s_cap.quad[0], s_cap.quad[2]);
		else
			pc_cap_push_tri(s_cap.quad[0], s_cap.quad[1], s_cap.quad[2]);
		if (s_cap.prim == 0x98) { // strip: keep last two
			for (int j = 0; j < 16; j++) { s_cap.quad[0][j] = s_cap.quad[1][j]; s_cap.quad[1][j] = s_cap.quad[2][j]; }
			s_cap.strip_odd++;
		} else { // fan: keep first + last
			for (int j = 0; j < 16; j++) s_cap.quad[1][j] = s_cap.quad[2][j];
		}
		s_cap.quadn = 2;
	}
	if (s_cap.got >= s_cap.expected) s_cap.active = 0;
}

unsigned long pc_gx_frame_tris_drawn;
extern "C" int pc_capture_pending(void); // pc_window.mm — F9 on-demand capture
extern "C" void pc_gx_frame_flush(void) {
	pc_gx_frame_tris_drawn = (unsigned long)(s_cap.outv / 3);
	// PC_DUMP_SEGS=frameno: one-shot dump of every segment's state + first vertex.
	// Also fires on demand when the capture key (F9) is pending — pc_window_frame
	// dumps the matching PNG the same frame, so the two line up.
	static long dump_at = -2;
	static long frame_no;
	frame_no++;
	if (dump_at == -2) dump_at = getenv("PC_DUMP_SEGS") ? atol(getenv("PC_DUMP_SEGS")) : -1;
	if (frame_no == dump_at || pc_capture_pending()) {
		// F9 capture writes a file too (pc_capture_segs.txt) so the user needn't
		// redirect stderr; PC_DUMP_SEGS keeps its stderr-only behavior.
		FILE* cf = pc_capture_pending() ? fopen("pc_capture_segs.txt", "w") : nullptr;
		for (int i = 0; i < s_nsegs; i++) {
			const float* v = &s_cap.out[s_segs[i].start * 16];
			// Screen-space bounds of this seg (x/w, y/w in NDC [-1,1]) so a draw
			// can be matched to a region of the captured frame.
			float xmin = 1e9f, xmax = -1e9f, ymin = 1e9f, ymax = -1e9f;
			float spmax = 0.0f; // brightest specular (chan-1) rgb across the seg
			// spavg distinguishes a TIGHT HIGHLIGHT (low avg, high max — GX-true
			// for glass/shiny) from a BROAD WASH (avg ~= max: our attenuation
			// lighting most of the surface, which washes tinted translucent
			// materials toward white through their spec-adding TEV stage).
			double spsum = 0.0;
			for (int k = 0; k < s_segs[i].count; k++) {
				const float* p = &s_cap.out[(s_segs[i].start + k) * 16];
				float w = p[3];
				if (w > 1e-4f || w < -1e-4f) {
					float sx = p[0] / w, sy = p[1] / w;
					if (sx < xmin) xmin = sx; if (sx > xmax) xmax = sx;
					if (sy < ymin) ymin = sy; if (sy > ymax) ymax = sy;
				}
				float sm = p[12] > p[13] ? (p[12] > p[14] ? p[12] : p[14]) : (p[13] > p[14] ? p[13] : p[14]);
				if (sm > spmax) spmax = sm;
				spsum += sm;
			}
			float spavg = s_segs[i].count ? (float)(spsum / s_segs[i].count) : 0.0f;
			int bc = (s_segs[i].depth >> 2) & 0x7f; // blendcode: 0x40|src<<3|dst
			const PcTevProg* P = &s_segs[i].prog;
			char line[640];
			// A[...] = the GX alpha-channel lighting inputs (GX_ALPHA0): lit
			// enable, light mask, ambient/material alpha, light 0/1 alpha
			// (-1 = slot never loaded). On GC a lit alpha channel outputs
			// mat_a * clamp(amb_a + sum(diffuse * light_a)); our bake outputs
			// mat_a raw — compare against ras[3] to quantify the gap.
			snprintf(line, sizeof(line), "[seg %02d] tex=%-3d n=%-5d nst=%d s0c(%d,%d,%d,%d)a(%d,%d,%d,%d) matsrc=%d/%d s0[uv%d chan%d tf%d] blend=%d(s%d,d%d) z=%d%d cull=%d atest=%d(c%d,r%d) spmax=%.2f spavg=%.2f ndc[x %.2f..%.2f y %.2f..%.2f] ras=%.2f,%.2f,%.2f,%.2f C0=%.2f,%.2f,%.2f,%.2f A[lit=%d m=%02x amb=%.2f mat=%.2f l0=%.2f l1=%.2f]\n",
			        i, s_segs[i].tex, s_segs[i].count, P->nstages,
			        P->cargs[0][0], P->cargs[0][1], P->cargs[0][2], P->cargs[0][3],
			        P->aargs[0][0], P->aargs[0][1], P->aargs[0][2], P->aargs[0][3], s_segs[i].matsrc & 1, (s_segs[i].matsrc >> 1) & 1,
			        P->tex[0][1], P->tex[0][2], P->tex[0][0],
			        bc, (bc & 0x40) ? (bc >> 3) & 7 : -1, (bc & 0x40) ? bc & 7 : -1,
			        s_segs[i].depth & 1, (s_segs[i].depth >> 1) & 1, (s_segs[i].depth >> 10) & 3,
			        (s_segs[i].depth >> 9) & 1, P->atest & 7, P->atest >> 3,
			        spmax, spavg, xmin, xmax, ymin, ymax, v[4], v[5], v[6], v[7],
			        P->regs[1][0], P->regs[1][1], P->regs[1][2], P->regs[1][3],
			        s_segs[i].alit, s_segs[i].lmask, s_segs[i].amba, s_segs[i].mata,
			        s_segs[i].la0, s_segs[i].la1);
			fputs(line, stderr);
			if (cf) fputs(line, cf);
			snprintf(line, sizeof(line), "         C1=%.2f,%.2f,%.2f,%.2f C2=%.2f,%.2f,%.2f,%.2f K=%.2f,%.2f,%.2f,%.2f\n",
			         P->regs[2][0], P->regs[2][1], P->regs[2][2], P->regs[2][3],
			         P->regs[3][0], P->regs[3][1], P->regs[3][2], P->regs[3][3],
			         P->konst[0][0], P->konst[0][1], P->konst[0][2], P->konst[0][3]);
			fputs(line, stderr);
			if (cf) fputs(line, cf);
			if (P->nstages > 1) { // multi-stage: konst values, regs, and EVERY stage's program
				snprintf(line, sizeof(line), "         K.a=%.2f,%.2f,%.2f,%.2f C1=%.2f,%.2f,%.2f,%.2f C2=%.2f,%.2f,%.2f,%.2f\n",
				         P->konst[0][3], P->konst[1][3], P->konst[2][3], P->konst[3][3],
				         P->regs[2][0], P->regs[2][1], P->regs[2][2], P->regs[2][3],
				         P->regs[3][0], P->regs[3][1], P->regs[3][2], P->regs[3][3]);
				fputs(line, stderr);
				if (cf) fputs(line, cf);
				for (int st = 1; st < P->nstages; st++) {
					snprintf(line, sizeof(line), "         s%d c(%d,%d,%d,%d)op(%d,%d,%d>%d) a(%d,%d,%d,%d)op(%d,%d,%d>%d) texf=%d texh=%d uv%d chan%d k%d/%d\n",
					         st, P->cargs[st][0], P->cargs[st][1], P->cargs[st][2], P->cargs[st][3],
					         P->cops[st][0], P->cops[st][1], P->cops[st][2], P->cops[st][3],
					         P->aargs[st][0], P->aargs[st][1], P->aargs[st][2], P->aargs[st][3],
					         P->aops[st][0], P->aops[st][1], P->aops[st][2], P->aops[st][3],
					         P->tex[st][0], P->misc[st][3], P->tex[st][1], P->tex[st][2],
					         P->tex[st][3], P->misc[st][2]);
					fputs(line, stderr);
					if (cf) fputs(line, cf);
				}
			}
		}
		if (cf) fclose(cf);
	}
	for (int i = 0; i < s_nsegs; i++)
		pc_window_submit_tris(&s_cap.out[s_segs[i].start * 16], s_segs[i].count, s_segs[i].tex, s_segs[i].depth, &s_segs[i].prog);
	{
		// PC_VTXDIAG=1: high-water mark of what the frame actually WANTED.
		static int vtxdiag = -1;
		if (vtxdiag < 0) vtxdiag = getenv("PC_VTXDIAG") ? 1 : 0;
		if (vtxdiag) {
			static int peak = 0;
			if (s_cap.wantv > peak) {
				peak = s_cap.wantv;
				fprintf(stderr, "[VTX] peak wanted=%d verts (capacity %d, %s)\n", peak, PC_CAP_MAX_VERTS,
				        peak > PC_CAP_MAX_VERTS ? "OVERFLOWING" : "ok");
			}
			// Segments have their own fixed cap, and overflow there is WORSE than the
			// vertex one: the new segment is skipped and the triangle is appended to
			// the previous, unrelated segment, so it draws with the wrong texture and
			// render state instead of vanishing. Tracked as a true per-frame
			// high-water mark, NOT sampled — PC_DIAG_DRAW only samples, and this
			// codebase has already been misled once by bisecting against sampled
			// output.
			static int segpeak = 0;
			if (s_nsegs_want > segpeak) {
				segpeak = s_nsegs_want;
				fprintf(stderr, "[VTX] peak segs wanted=%d (capacity %d, %s)\n", segpeak, PC_CAP_MAX_SEGS,
				        segpeak > PC_CAP_MAX_SEGS ? "OVERFLOWING" : "ok");
			}
		}
	}
	s_cap.wantv = 0;
	s_cap.outv = 0;
	s_nsegs = 0;
	s_nsegs_want = 0;
}

GXFifoObj* GXInit(void* base, u32 size) {
	(void)base; (void)size;
	static GXFifoObj fifo;
	memset((void*)&fifo, 0, sizeof(fifo));
	memcpy(s_cap.pos34, s_ident34, sizeof(s_ident34));
	for (int i = 0; i < 16; i++) memcpy(s_cap.mtxpal[i], s_ident34, sizeof(s_ident34));
	for (int i = 0; i < 16; i++) memcpy(s_cap.nrmpal[i], s_ident34, sizeof(s_ident34));
	s_cap.vtx_mtx_slot = -1;
	s_cap.curTex = -1;
	return &fifo;
}

void GXBegin(GXPrimitive type, GXVtxFmt fmt, u16 nverts) {
	pc_gx_frame_begins++;
	pc_gx_frame_verts += nverts;
	// Build the per-vertex scalar plan from the live descriptor state.
	s_cap.nattrs = 0;
	for (int a = 0; a < 26; a++) {
		if (!s_vtxdesc[a]) continue;
		int scal = 0;
		if (s_vtxdesc[a] == 1) { // GX_DIRECT
			if (a == 9) scal = (s_fmt_cnt[fmt][a] == 0) ? 2 : 3;      // POS: XY/XYZ
			else if (a == 10) scal = (s_fmt_cnt[fmt][a] == 0) ? 3 : 9; // NRM: XYZ / NBT
			else if (a == 11 || a == 12) scal = 4;                     // CLR via u8x4 (u32 handled specially)
			else if (a >= 13 && a <= 20) scal = (s_fmt_cnt[fmt][a] == 0) ? 1 : 2; // TEXn
			else scal = 1;                                             // matrix indices etc.
		} else {
			scal = 1; // indexed: one index scalar (positions-by-index unsupported yet)
		}
		if (s_cap.nattrs < PC_MAX_ATTRS) {
			s_cap.attr[s_cap.nattrs] = a;
			s_cap.scalars[s_cap.nattrs] = scal;
			s_cap.nattrs++;
		} else {
			// GX allows 21 enabled attributes (PNMTXIDX + 8 TEXnMTXIDX + POS +
			// NRM + 2 CLR + 8 TEX), so this cap is reachable in principle. Losing
			// one is not a dropped vertex, it is a DESYNCED scalar stream: the
			// game still writes the attribute's scalars into the FIFO and the
			// plan no longer accounts for them, so every vertex after it is
			// misparsed. Loud, once — silence here would read as a shader bug.
			static int warned = 0;
			if (!warned) {
				warned = 1;
				fprintf(stderr, "[GX] *** vertex attribute plan FULL at %d - attr %d dropped, the scalar stream "
				                "will DESYNC and all geometry misparse. Raise PC_MAX_ATTRS.\n",
				        PC_MAX_ATTRS, a);
			}
		}
	}
	s_cap.active = s_cap.nattrs > 0;
	s_cur_fmt = fmt;
	s_cap.prim = type;
	s_cap.expected = nverts;
	s_cap.got = 0;
	s_cap.cursor = 0;
	s_cap.filled = 0;
	s_cap.quadn = 0;
	s_cap.strip_odd = 0;
	// default color: material register when the layout carries no CLR0/CLR1
	// (channel outputs GX_SRC_REG), else white until the vertex supplies it
	extern float pc_gx_chan_mat[4];
	if (!s_vtxdesc[11] && !s_vtxdesc[12]) {
		for (int i = 0; i < 4; i++) s_cap.clr[i] = pc_gx_chan_mat[i];
	} else {
		s_cap.clr[0] = s_cap.clr[1] = s_cap.clr[2] = s_cap.clr[3] = 1.0f;
	}
	if (!s_vtxdesc[0]) s_cap.vtx_mtx_slot = -1; // no PNMTXIDX attr -> current mtx
	s_cap.has_nrm = (s_vtxdesc[10] || s_vtxdesc[25]) ? 1 : 0;
	s_cap.nrm[0] = 0; s_cap.nrm[1] = 0; s_cap.nrm[2] = 1;
}

void GXFlush() { }
void GXDrawDone() { pc_gx_frame_draws++; }
void GXPixModeSync() { }
void GXInvalidateTexAll() { }
void GXInvalidateVtxCache() { }
struct OSThread* GXSetCurrentGXThread() { return nullptr; }

void GXBeginDisplayList(void*, u32) { }
u32 GXEndDisplayList() { return 0; }

// Interpreter for pre-authored mesh display lists (.mod DispList data):
// big-endian GX FIFO bytes — NOPs + draw commands whose vertex layout
// follows the live VtxDesc/AttrFmt state (dgxGraphics sets those first).
void GXCallDisplayList(void* data, u32 nbytes) {
	const u8* p = (const u8*)data;
	const u8* end = p + nbytes;
	pc_gx_in_dl = 1;
	while (p < end) {
		u8 op = *p++;
		if (op == 0x00) continue;                  // NOP
		if (op == 0x61) { p += 5; continue; }      // BP load
		if (op == 0x08) { p += 5; continue; }      // CP load (reg + u32)
		if (op == 0x10) {                          // XF load: (count-1)<<16|addr, then count words
			if (p + 4 > end) break;
			int cnt = ((p[0] << 8) | p[1]) + 1;
			p += 4 + cnt * 4;
			continue;
		}
		if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) { pc_gx_dl_indx_ops++; p += 4; continue; } // LOAD_INDX A-D
		if (op >= 0x80 && op < 0xC0) {             // draw primitive
			if (p + 2 > end) break;
			int prim = op & 0xF8, vat = op & 0x07;
			int count = (p[0] << 8) | p[1];
			p += 2;
			GXBegin((GXPrimitive)prim, (GXVtxFmt)vat, (u16)count);
			for (int v = 0; v < count && p < end; v++) {
				for (int a = 0; a < s_cap.nattrs; a++) {
					int attr = s_cap.attr[a];
					int desc = s_vtxdesc[attr];
					if (desc == 2) { // INDEX8
						pc_cap_scalar((float)*p++, 0);
					} else if (desc == 3) { // INDEX16
						pc_cap_scalar((float)((p[0] << 8) | p[1]), 0);
						p += 2;
					} else { // DIRECT: read scalars[a] components big-endian
						int t = s_fmt_type[vat][attr];
						int n = s_cap.scalars[a];
						for (int c = 0; c < n; c++) {
							if (attr == 11 || attr == 12) { // color: RGBA8/RGB8 bytes
								pc_cap_scalar((float)*p++, 0);
								if (t != 5 && c == 2) { // RGB8: synthesize alpha
									pc_cap_scalar(255.0f, 0);
									c++;
								}
							} else if (t == 4) {
								union { u32 u; float f; } cv;
								cv.u = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
								p += 4;
								pc_cap_scalar(cv.f, 0);
							} else if (t == 3) {
								pc_cap_scalar((float)(s16)((p[0] << 8) | p[1]), 1);
								p += 2;
							} else if (t == 2) {
								pc_cap_scalar((float)(u16)((p[0] << 8) | p[1]), 1);
								p += 2;
							} else if (t == 1) {
								pc_cap_scalar((float)(s8)*p++, 1);
							} else {
								pc_cap_scalar((float)*p++, 1);
							}
						}
					}
				}
			}
			continue;
		}
		// Unknown opcode: mesh DLs shouldn't contain these; stop parsing.
		static int warned;
		if (!warned++) {
			fprintf(stderr, "[GXdl] unknown opcode 0x%02x at +%d — DL abandoned\n", op, (int)(p - 1 - (const u8*)data));
			fprintf(stderr, "[GXdl] plan:");
			for (int a = 0; a < s_cap.nattrs; a++)
				fprintf(stderr, " attr%d(desc=%d,scal=%d,type=%d)", s_cap.attr[a], s_vtxdesc[s_cap.attr[a]],
				        s_cap.scalars[a], s_fmt_type[s_cur_fmt][s_cap.attr[a]]);
			fprintf(stderr, "\n[GXdl] head:");
			const u8* q = (const u8*)data;
			for (int i = 0; i < 48 && q + i < end; i++) fprintf(stderr, " %02x", q[i]);
			fprintf(stderr, "\n");
		}
		break;
	}
	pc_gx_in_dl = 0;
}

void GXClearVtxDesc() { memset(s_vtxdesc, 0, sizeof(s_vtxdesc)); }
void GXSetVtxDesc(GXAttr attr, GXAttrType type) {
	if ((int)attr >= 0 && (int)attr < 26) s_vtxdesc[attr] = type;
}
void GXSetVtxDescv(GXVtxDescList* list) {
	// Batched descriptor set — REPLACES the whole descriptor state (this is
	// how dgxGraphics configures mesh layouts before GXCallDisplayList).
	memset(s_vtxdesc, 0, sizeof(s_vtxdesc));
	for (int i = 0; list && i < 26; i++) {
		if ((int)list[i].attr == 0xFF /* GX_VA_NULL */) break;
		if ((int)list[i].attr >= 0 && (int)list[i].attr < 26) s_vtxdesc[list[i].attr] = list[i].type;
	}
}
void GXSetVtxAttrFmt(GXVtxFmt fmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac) {
	if ((int)fmt < 8 && (int)attr >= 0 && (int)attr < 26) {
		s_fmt_cnt[fmt][attr] = cnt;
		s_fmt_frac[fmt][attr] = frac;
		s_fmt_type[fmt][attr] = type;
	}
}
void GXSetVtxAttrFmtv(GXVtxFmt fmt, GXVtxAttrFmtList* list) {
	for (int i = 0; list && (int)fmt < 8 && i < 26; i++) {
		if ((int)list[i].attr == 0xFF) break;
		int a = list[i].attr;
		if (a >= 0 && a < 26) {
			s_fmt_cnt[fmt][a] = list[i].count;
			s_fmt_frac[fmt][a] = list[i].frac;
			s_fmt_type[fmt][a] = list[i].type;
		}
	}
}
void GXSetArray(GXAttr attr, void* base, u8 stride) {
	if ((int)attr >= 0 && (int)attr < 26) {
		s_array_base[attr] = base;
		s_array_stride[attr] = stride;
	}
}

void GXSetNumChans(u8) { }
// Color-channel state: with no vertex-color attribute the channel outputs
// the material color register (GX_SRC_REG).
float pc_gx_chan_mat[4] = { 1, 1, 1, 1 };
// Lighting v1: per-vertex diffuse from the channel-0 light mask (state
// declared above pc_cap_emit_vertex). Approximations: DF_CLAMP diffuse,
// no spot/dist attenuation, slot 7 (specular) skipped.
static PCLightObj* pc_light(GXLightObj* o) { return (PCLightObj*)o; }
void GXSetChanCtrl(GXChannelID chan, GXBool enable, GXColorSrc, GXColorSrc matSrc, u32 mask, GXDiffuseFn, GXAttnFn) {
	// matSrc=GX_SRC_REG means that component comes from the material register
	// (GXSetChanMatColor) rather than the vertex. P2DPicture carries its fade in
	// the register alpha, so honouring it is what makes panels transparent —
	// but colour and alpha must be honoured INDEPENDENTLY (see above).
	if (chan == 0 /* GX_COLOR0 */ || chan == 4 /* GX_COLOR0A0 */) {
		pc_gx_chan0_lit = enable ? 1 : 0;
		s_chan0_mask = mask & 0x7F; // slot 7 is the specular channel's light
		s_chan0_matsrc_c = (int)matSrc;
	}
	if (chan == 2 /* GX_ALPHA0 */ || chan == 4 /* GX_COLOR0A0 */) {
		s_chan0_matsrc = (int)matSrc; // alpha source
		s_chan0_alpha_lit = enable ? 1 : 0;
	}
	if (chan == 1 /* GX_COLOR1 */ || chan == 5 /* GX_COLOR1A1 */) {
		pc_gx_chan1_lit = enable ? 1 : 0;
		s_chan1_mask = mask; // 0x80: the specular light in slot 7
	}
}
void GXSetChanAmbColor(GXChannelID chan, GXColor c) {
	if (chan == 0 || chan == 4) {
		pc_gx_chan_amb[0] = c.r / 255.0f;
		pc_gx_chan_amb[1] = c.g / 255.0f;
		pc_gx_chan_amb[2] = c.b / 255.0f;
	}
	// ambient ALPHA rides GX_ALPHA0/GX_COLOR0A0 only (setAmbient passes
	// COLOR0A0 with a = mAmbientColour.a * mLightIntensity)
	if (chan == 2 || chan == 4) pc_gx_chan_amb[3] = c.a / 255.0f;
	if (chan == 1 || chan == 5) {
		pc_gx_chan1_amb[0] = c.r / 255.0f;
		pc_gx_chan1_amb[1] = c.g / 255.0f;
		pc_gx_chan1_amb[2] = c.b / 255.0f;
	}
}
void GXSetChanMatColor(GXChannelID chan, GXColor c) {
	if (chan == 0 /* GX_COLOR0 */ || chan == 4 /* GX_COLOR0A0 */) {
		pc_gx_chan_mat[0] = c.r / 255.0f;
		pc_gx_chan_mat[1] = c.g / 255.0f;
		pc_gx_chan_mat[2] = c.b / 255.0f;
	}
	if (chan == 2 /* GX_ALPHA0 */ || chan == 4 /* GX_COLOR0A0 */) pc_gx_chan_mat[3] = c.a / 255.0f;
	if (chan == 1 /* GX_COLOR1 */ || chan == 5 /* GX_COLOR1A1 */) {
		pc_gx_chan1_mat[0] = c.r / 255.0f;
		pc_gx_chan1_mat[1] = c.g / 255.0f;
		pc_gx_chan1_mat[2] = c.b / 255.0f;
	}
	if (chan == 3 /* GX_ALPHA1 */ || chan == 5 /* GX_COLOR1A1 */) pc_gx_chan1_mat[3] = c.a / 255.0f;
}

static PCLightObj* pc_light_init(GXLightObj* o) {
	PCLightObj* l = pc_light(o);
	if (l->magic != PC_LIGHT_MAGIC) {
		memset(l, 0, sizeof(*l));
		l->magic = PC_LIGHT_MAGIC;
		l->r = l->g = l->b = l->a = 1.0f;
	}
	return l;
}
void GXInitLightAttn(GXLightObj* o, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2) {
	PCLightObj* l = pc_light_init(o);
	l->aattn[0] = a0; l->aattn[1] = a1; l->aattn[2] = a2;
	l->kattn[0] = k0; l->kattn[1] = k1; l->kattn[2] = k2;
}
void GXInitLightColor(GXLightObj* o, GXColor c) {
	PCLightObj* l = pc_light_init(o);
	l->r = c.r / 255.0f; l->g = c.g / 255.0f; l->b = c.b / 255.0f; l->a = c.a / 255.0f;
}
void GXInitLightDir(GXLightObj* o, f32 x, f32 y, f32 z) {
	PCLightObj* l = pc_light_init(o);
	l->dir[0] = x; l->dir[1] = y; l->dir[2] = z;
}
void GXInitLightPos(GXLightObj* o, f32 x, f32 y, f32 z) {
	PCLightObj* l = pc_light_init(o);
	l->pos[0] = x; l->pos[1] = y; l->pos[2] = z;
	l->pad[0] = 0; // a later GXInitSpecularDir re-marks the object as specular
}
void GXInitSpecularDir(GXLightObj* o, f32 x, f32 y, f32 z) {
	// SDK-faithful (src/gx/GXLight.c): dir <- the HALF-ANGLE vector
	// normalize(-d + (0,0,1)), pos <- -d * 2^20 (points toward the light; its
	// normalization is the shine gate). pad[0] flags the object as specular.
	PCLightObj* l = pc_light_init(o);
	float vx = -x, vy = -y, vz = -z + 1.0f;
	float mag = sqrtf(vx * vx + vy * vy + vz * vz);
	if (mag > 1e-6f) { vx /= mag; vy /= mag; vz /= mag; }
	l->dir[0] = vx; l->dir[1] = vy; l->dir[2] = vz;
	l->pos[0] = -x * 1048576.0f; l->pos[1] = -y * 1048576.0f; l->pos[2] = -z * 1048576.0f;
	l->pad[0] = 1;
}
void GXLoadLightObjImm(GXLightObj* o, GXLightID id) {
	PCLightObj* l = pc_light_init(o);
	for (int i = 0; i < 8; i++)
		if ((u32)id & (1u << i)) s_lights[i] = *l;
	if (getenv("PC_LIGHT_TRACE")) {
		static u32 seen[64]; static int nseen;
		u32 key = (u32)id * 2654435761u ^ ((u32)(l->pos[0] * 7) + (u32)(l->dir[0] * 13) + (u32)(l->r * 255) * 131);
		bool found = false;
		for (int i = 0; i < nseen; i++) if (seen[i] == key) { found = true; break; }
		if (!found && nseen < 64) {
			seen[nseen++] = key;
			fprintf(stderr, "[LIGHT] id=0x%x pos=(%.1f,%.1f,%.1f) dir=(%.2f,%.2f,%.2f) rgb=(%.2f,%.2f,%.2f) amb=(%.2f,%.2f,%.2f) lit=%d mask=0x%x\n",
			        (u32)id, l->pos[0], l->pos[1], l->pos[2], l->dir[0], l->dir[1], l->dir[2], l->r, l->g, l->b,
			        pc_gx_chan_amb[0], pc_gx_chan_amb[1], pc_gx_chan_amb[2], pc_gx_chan0_lit, s_chan0_mask);
		}
	}
}

void GXLoadPosMtxImm(const Mtx mtx, u32 id) {
	u32 slot = id / 3;
	pc_gx_posmtx_loads++;
	if ((int)slot > pc_gx_posmtx_maxslot) pc_gx_posmtx_maxslot = (int)slot;
	if (slot < 16) memcpy(s_cap.mtxpal[slot], mtx, sizeof(f32) * 12);
}
void GXLoadNrmMtxImm(const Mtx mtx, u32 id) {
	u32 slot = id / 3;
	if (slot < 16) memcpy(s_cap.nrmpal[slot], mtx, sizeof(f32) * 12);
}
void GXLoadTexMtxImm(const Mtx mtx, u32 id, GXTexMtxType) {
	if (id >= 30 && id <= 57) {
		int slot = (id - 30) / 3;
		memcpy(s_texmtx[slot], mtx, sizeof(f32) * 8); // top 2 rows
	}
}
void GXSetCurrentMtx(u32 id) { s_cap.cur_mtx_slot = (int)(id / 3); }

// ------------------------------------------------------------ textures
// GC tiled-format decode -> RGBA8 -> renderer texture, cached per image
// pointer. Pikmin uses no CI/palette formats (see gxTexFmts[] in texture.cpp:
// RGB565/CMPR/RGB5A3/I4/I8/IA4/IA8/RGBA8/Z8); anything else -> magenta.
// Store our info INSIDE the opaque 32-byte GXTexObj (as the real GX does):
// the game memcpys texobjs and rebuilds them on the stack, so any
// pointer-keyed side table returns stale/foreign entries (symptom: every
// unmatched draw bound the first-ever texture — the kanji console font).
// 32-byte budget (fits inside GXTexObj): handle caches the decoded GPU
// texture PER TEXOBJ — GXInitTexObj resets it, so when the texture cacher
// recycles a RAM window address for new pixels, re-attach naturally
// re-decodes (a global pointer-keyed cache returned stale handles there).
struct PCTexInfo {
	void* image;
	u16 w, h;
	u16 fmt, wrapS, wrapT;
	u16 mip; // GXInitTexObj's mipmap flag: authored mip chain -> trilinear
	s32 handle;
	u32 magic;
};
#define PC_TEXOBJ_MAGIC 0x50435458u // 'PCTX'
static PCTexInfo* pc_texobj(GXTexObj* o) { return (PCTexInfo*)o; }

static u32 pc_dec_rgb5a3(u16 p) {
	u8 r, g, b, a;
	if (p & 0x8000) {
		r = (p >> 10) & 0x1F; r = (r << 3) | (r >> 2);
		g = (p >> 5) & 0x1F;  g = (g << 3) | (g >> 2);
		b = p & 0x1F;         b = (b << 3) | (b >> 2);
		a = 255;
	} else {
		a = (p >> 12) & 0x7;  a = (a << 5) | (a << 2) | (a >> 1);
		r = (p >> 8) & 0xF;   r = (r << 4) | r;
		g = (p >> 4) & 0xF;   g = (g << 4) | g;
		b = p & 0xF;          b = (b << 4) | b;
	}
	return (u32)r | ((u32)g << 8) | ((u32)b << 16) | ((u32)a << 24);
}


// ------------------------------------------------------ HD texture packs
// Dolphin-format packs, looked up exactly the way Dolphin does it — no manifest
// and no matching step. From VideoCommon/TextureInfo.cpp the name is
//
//     tex1_<w>x<h>_<XXH64(data, texture_size, 0):016x>_<format>[.dds|.png]
//
// where texture_size is expanded_w * expanded_h * texel_nibbles / 2. Note the
// hash in the FILENAME is plain XXH64 seed 0 — the CRC32-based, CPU-dispatched,
// sample-count-dependent routine in Common/Hash.cpp belongs to the texture
// CACHE and is a different thing. Confusing the two costs an afternoon.
//
// PC_HD_TEXTURES=<dir> is scanned recursively at startup; otherwise
// <user data>/hd. A miss, an unreadable file or an unsupported DDS variant all
// fall through to the game's own texture, so this cannot break rendering.
int pc_window_upload_dds(const char* path, int wrapS, int wrapT);          // pc_window.mm
int pc_window_upload_png(const char* path, int wrapS, int wrapT, int mip); // pc_window.mm
extern "C" const char* pc_path_support(void);                             // pc_paths.cpp

// XXH64, seed 0. Matches Dolphin's filename hash exactly.
static inline u64 pc_rotl64(u64 x, int r) { return (x << r) | (x >> (64 - r)); }
static u64 pc_xxh64(const u8* p, size_t len) {
	const u64 P1 = 11400714785074694791ull, P2 = 14029467366897019727ull;
	const u64 P3 = 1609587929392839161ull,  P4 = 9650029242287828579ull;
	const u64 P5 = 2870177450012600261ull;
	const u8* const end = p + len;
	u64 h;
	auto rd64 = [](const u8* q) {
		u64 v; memcpy(&v, q, 8); return v; // little-endian host
	};
	auto rd32 = [](const u8* q) {
		u32 v; memcpy(&v, q, 4); return (u64)v;
	};
	auto round = [&](u64 acc, u64 in) { acc += in * P2; acc = pc_rotl64(acc, 31); return acc * P1; };

	if (len >= 32) {
		const u8* const limit = end - 32;
		u64 v1 = P1 + P2, v2 = P2, v3 = 0, v4 = 0ull - P1;
		do {
			v1 = round(v1, rd64(p)); p += 8;
			v2 = round(v2, rd64(p)); p += 8;
			v3 = round(v3, rd64(p)); p += 8;
			v4 = round(v4, rd64(p)); p += 8;
		} while (p <= limit);
		h = pc_rotl64(v1, 1) + pc_rotl64(v2, 7) + pc_rotl64(v3, 12) + pc_rotl64(v4, 18);
		auto merge = [&](u64 acc, u64 v) { return (acc ^ round(0, v)) * P1 + P4; };
		h = merge(h, v1); h = merge(h, v2); h = merge(h, v3); h = merge(h, v4);
	} else {
		h = P5;
	}
	h += (u64)len;
	while (p + 8 <= end) { h ^= round(0, rd64(p)); h = pc_rotl64(h, 27) * P1 + P4; p += 8; }
	if (p + 4 <= end)    { h ^= rd32(p) * P1;      h = pc_rotl64(h, 23) * P2 + P3; p += 4; }
	while (p < end)      { h ^= (u64)(*p++) * P5;  h = pc_rotl64(h, 11) * P1; }
	h ^= h >> 33; h *= P2; h ^= h >> 29; h *= P3; h ^= h >> 32;
	return h;
}

// Bytes a tiled GC texture occupies, matching Dolphin's texture_size:
// AlignUp to the block grid, then width*height*nibbles/2.
static long pc_gc_tex_bytes(int w, int h, int fmt) {
	int bw, bh, nibbles;
	switch (fmt) {
	case 0x0: bw = 8; bh = 8; nibbles = 1; break;  // I4
	case 0x1: bw = 8; bh = 4; nibbles = 2; break;  // I8
	case 0x2: bw = 8; bh = 4; nibbles = 2; break;  // IA4
	case 0x3: bw = 4; bh = 4; nibbles = 4; break;  // IA8
	case 0x4: bw = 4; bh = 4; nibbles = 4; break;  // RGB565
	case 0x5: bw = 4; bh = 4; nibbles = 4; break;  // RGB5A3
	case 0x6: bw = 4; bh = 4; nibbles = 8; break;  // RGBA8
	case 0xE: bw = 8; bh = 8; nibbles = 1; break;  // CMPR
	default: return 0;
	}
	const long ew = ((w + bw - 1) / bw) * bw, eh = ((h + bh - 1) / bh) * bh;
	return ew * eh * nibbles / 2;
}

struct PcHdEntry { u64 key; char path[512]; };
static PcHdEntry* s_hd;
static int s_nhd, s_hd_cap;

static void pc_hd_add(const char* dir, const char* name) {
	// tex1_<w>x<h>_<hash>_<fmt>.<ext>. Skip Dolphin's extra variants: _mipN are
	// mip levels of a base entry, and .mat.dds are MATERIAL maps for its shader
	// system, not colour replacements.
	if (strncmp(name, "tex1_", 5) != 0) return;
	if (strstr(name, "_mip") || strstr(name, ".mat.")) return;
	const char* p = strchr(name + 5, '_');
	if (!p) return;
	u64 key = 0;
	int n    = 0;
	for (const char* q = p + 1; *q && *q != '_'; q++, n++) {
		const char c = *q;
		const int d  = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
		if (d < 0) return;
		key = (key << 4) | (u64)d;
	}
	if (n != 16) return;
	if (s_nhd == s_hd_cap) {
		s_hd_cap = s_hd_cap ? s_hd_cap * 2 : 256;
		PcHdEntry* g = (PcHdEntry*)realloc(s_hd, sizeof(PcHdEntry) * s_hd_cap);
		if (!g) return;
		s_hd = g;
	}
	s_hd[s_nhd].key = key;
	snprintf(s_hd[s_nhd].path, sizeof(s_hd[s_nhd].path), "%s/%s", dir, name);
	s_nhd++;
}

static void pc_hd_scan(const char* dir, int depth) {
	if (depth > 4) return;
	DIR* d = opendir(dir);
	if (!d) return;
	struct dirent* e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char full[1024];
		snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
		struct stat st;
		if (stat(full, &st) != 0) continue;
		if (S_ISDIR(st.st_mode)) pc_hd_scan(full, depth + 1);
		else pc_hd_add(dir, e->d_name);
	}
	closedir(d);
}

static void pc_hd_init(void) {
	static int done;
	if (done) return;
	done = 1;
	char root[1024];
	const char* env = getenv("PC_HD_TEXTURES");
	if (env && *env) snprintf(root, sizeof(root), "%s", env);
	else snprintf(root, sizeof(root), "%s/hd", pc_path_support());
	pc_hd_scan(root, 0);
	if (s_nhd) fprintf(stderr, "[HD] %d replacement textures under %s\n", s_nhd, root);
}

// Returns a texture handle, or -1 to fall through to the normal decode.
static int pc_hd_lookup(const u8* img, int w, int h, int fmt, int wrapS, int wrapT, int mip) {
	pc_hd_init();
	if (!s_nhd) return -1;
	const long bytes = pc_gc_tex_bytes(w, h, fmt);
	if (!bytes) return -1;
	const u64 key = pc_xxh64(img, (size_t)bytes);
	for (int i = 0; i < s_nhd; i++) {
		if (s_hd[i].key != key) continue;
		const size_t n   = strlen(s_hd[i].path);
		const bool isdds = n > 4 && strcmp(s_hd[i].path + n - 4, ".dds") == 0;
		const int handle = isdds ? pc_window_upload_dds(s_hd[i].path, wrapS, wrapT)
		                         : pc_window_upload_png(s_hd[i].path, wrapS, wrapT, mip);
		static int reported;
		if (reported < 12) {
			reported++;
			fprintf(stderr, "[HD] %dx%d fmt=0x%x -> %s%s\n", w, h, fmt, strrchr(s_hd[i].path, '/') + 1,
			        handle < 0 ? "  (load failed, using original)" : "");
		}
		return handle;
	}
	return -1;
}

static void pc_decode_gc_tex(const u8* src, int w, int h, int fmt, u32* out) {
	int tw = 8, th = 8, bpp4 = 0; // tile dims
	switch (fmt) {
	case 0x0: tw = 8; th = 8; break;  // I4
	case 0x1: tw = 8; th = 4; break;  // I8
	case 0x11: tw = 8; th = 4; break; // Z8 (8-bit Z) — 1 byte/texel, decode as grayscale like I8
	case 0x2: tw = 8; th = 4; break;  // IA4
	case 0x3: tw = 4; th = 4; break;  // IA8
	case 0x4: tw = 4; th = 4; break;  // RGB565
	case 0x5: tw = 4; th = 4; break;  // RGB5A3
	case 0x6: tw = 4; th = 4; break;  // RGBA8
	case 0xE: tw = 8; th = 8; break;  // CMPR
	default: {
		// Magenta is the "we do not decode this format" placeholder. It is a
		// deliberate loud failure, but it never said WHICH format, which makes a
		// magenta screen a dead end to debug. Report each unknown format once.
		static u32 reported;
		if (fmt < 32 && !(reported & (1u << fmt))) {
			reported |= 1u << fmt;
			fprintf(stderr, "[GX] UNSUPPORTED texture format 0x%x (%dx%d) -> magenta placeholder\n", fmt, w, h);
		} else if (fmt >= 32) {
			static int oob;
			if (!oob) {
				oob = 1;
				fprintf(stderr, "[GX] texture format 0x%x out of range (%dx%d) -> magenta placeholder\n", fmt, w, h);
			}
		}
		for (int i = 0; i < w * h; i++) out[i] = 0xFFFF00FF; // magenta
		return;
	}
	}
	(void)bpp4;
	const u8* p = src;
	for (int ty = 0; ty < h; ty += th)
		for (int tx = 0; tx < w; tx += tw) {
			if (fmt == 0xE) { // CMPR: 4 DXT1-ish 4x4 sub-blocks, big-endian colors
				for (int sb = 0; sb < 4; sb++) {
					int bx = tx + (sb & 1) * 4, by = ty + (sb >> 1) * 4;
					u16 c0 = (u16)((p[0] << 8) | p[1]), c1 = (u16)((p[2] << 8) | p[3]);
					u32 bits = ((u32)p[4] << 24) | ((u32)p[5] << 16) | ((u32)p[6] << 8) | p[7];
					p += 8;
					u32 pal[4];
					u8 r0 = (c0 >> 11) << 3, g0 = ((c0 >> 5) & 0x3F) << 2, b0 = (c0 & 0x1F) << 3;
					u8 r1 = (c1 >> 11) << 3, g1 = ((c1 >> 5) & 0x3F) << 2, b1 = (c1 & 0x1F) << 3;
					pal[0] = (u32)r0 | ((u32)g0 << 8) | ((u32)b0 << 16) | 0xFF000000u;
					pal[1] = (u32)r1 | ((u32)g1 << 8) | ((u32)b1 << 16) | 0xFF000000u;
					if (c0 > c1) {
						pal[2] = (u32)((2 * r0 + r1) / 3) | ((u32)((2 * g0 + g1) / 3) << 8) | ((u32)((2 * b0 + b1) / 3) << 16) | 0xFF000000u;
						pal[3] = (u32)((r0 + 2 * r1) / 3) | ((u32)((g0 + 2 * g1) / 3) << 8) | ((u32)((b0 + 2 * b1) / 3) << 16) | 0xFF000000u;
					} else {
						pal[2] = (u32)((r0 + r1) / 2) | ((u32)((g0 + g1) / 2) << 8) | ((u32)((b0 + b1) / 2) << 16) | 0xFF000000u;
						pal[3] = 0;
					}
					for (int y = 0; y < 4; y++)
						for (int x = 0; x < 4; x++) {
							int px = bx + x, py = by + y;
							if (px < w && py < h)
								out[py * w + px] = pal[(bits >> (30 - 2 * (y * 4 + x))) & 3];
						}
				}
				continue;
			}
			for (int y = 0; y < th; y++)
				for (int x = 0; x < tw; x++) {
					int px = tx + x, py = ty + y;
					u32 c = 0;
					switch (fmt) {
					case 0x0: { u8 v = p[(y * tw + x) / 2]; v = (x & 1) ? (v & 0xF) : (v >> 4); v = (u8)((v << 4) | v);
						c = (u32)v | ((u32)v << 8) | ((u32)v << 16) | ((u32)v << 24); break; }
					// I8: GX expands intensity into all four channels (IIII), so
					// alpha = intensity — the same rule as I4 above. This is what
					// makes intensity-only smoke/cloud/shadow textures blend out
					// their black (alpha 0) under SRCALPHA blend; hardcoding
					// alpha=0xFF left every dark texel opaque.
					case 0x1: { u8 v = p[y * tw + x]; c = (u32)v | ((u32)v << 8) | ((u32)v << 16) | ((u32)v << 24); break; }
					// Z8: an 8-bit depth grab decoded as grayscale; keep it opaque.
					case 0x11: { u8 v = p[y * tw + x]; c = (u32)v | ((u32)v << 8) | ((u32)v << 16) | 0xFF000000u; break; }
					case 0x2: { u8 v = p[y * tw + x]; u8 i = (u8)((v & 0xF) * 17), a = (u8)((v >> 4) * 17);
						c = (u32)i | ((u32)i << 8) | ((u32)i << 16) | ((u32)a << 24); break; }
					// IA8: byte 0 = ALPHA, byte 1 = INTENSITY. Three
					// independent lines of evidence (v52): Dolphin's texture
					// dumps for every IA8 in the game pair with ours only in
					// this orientation; a plane that is CONSTANT 255 can only be
					// intensity (a flat white sprite) and 36 of our textures had
					// that plane in alpha under the other order ("fully opaque
					// sheen" — not a thing); and the user's rendered overlay vs
					// Dolphin showed darks/lights exactly INVERTED.
					// v47 briefly swapped these because it made the helmet look
					// right — but that was masking a different bug (the shine
					// pass computes TEXC + RASC*TEXA, and our RASC/material
					// register RGB is white where GC has a dark tint, so the
					// broad alpha mask leaks into the COLOUR as a milky veil).
					// Fix the register, not the decode. PC_IA8_SWAP=1 restores
					// the v47 order for A/B while that is outstanding.
					// (IA4, case 0x2, is genuinely the other way round: low
					// nibble intensity, high nibble alpha.)
					case 0x3: { const u8* q = &p[(y * tw + x) * 2];
						static int s_ia8_swap = -1;
						if (s_ia8_swap < 0) s_ia8_swap = getenv("PC_IA8_SWAP") ? 1 : 0;
						u8 a = s_ia8_swap ? q[1] : q[0];
						u8 i = s_ia8_swap ? q[0] : q[1];
						c = (u32)i | ((u32)i << 8) | ((u32)i << 16) | ((u32)a << 24); break; }
					case 0x4: { const u8* q = &p[(y * tw + x) * 2]; u16 v = (u16)((q[0] << 8) | q[1]);
						u8 r = (u8)((v >> 11) << 3), g = (u8)(((v >> 5) & 0x3F) << 2), b = (u8)((v & 0x1F) << 3);
						c = (u32)r | ((u32)g << 8) | ((u32)b << 16) | 0xFF000000u; break; }
					case 0x5: { const u8* q = &p[(y * tw + x) * 2]; c = pc_dec_rgb5a3((u16)((q[0] << 8) | q[1])); break; }
					case 0x6: { const u8* ar = &p[(y * 4 + x) * 2], *gb = &p[32 + (y * 4 + x) * 2];
						c = (u32)ar[1] | ((u32)gb[0] << 8) | ((u32)gb[1] << 16) | ((u32)ar[0] << 24); break; }
					}
					if (px < w && py < h) out[py * w + px] = c;
				}
			switch (fmt) {
			case 0x0: p += 32; break;
			case 0x1: case 0x11: case 0x2: case 0x4: case 0x5: p += 32; break;
			case 0x3: p += 32; break;
			case 0x6: p += 64; break;
			}
		}
}

void GXInitTexObj(GXTexObj* obj, void* image, u16 w, u16 h, GXTexFmt fmt, GXTexWrapMode wrapS, GXTexWrapMode wrapT, GXBool mipmap) {
	PCTexInfo* t = pc_texobj(obj);
	t->image = image;
	t->w = w;
	t->h = h;
	t->fmt = (u16)fmt;
	t->wrapS = (u16)wrapS;
	t->wrapT = (u16)wrapT;
	// GC textures authored WITH a mip chain sample GX_LIN_MIP_LIN (trilinear,
	// texture.cpp GXInitTexObjLOD). Mip0-only sampling renders minified
	// geometry crisp/bright where GC shows deep-mip blur (e.g. the warp-exit
	// ribbons: hairlines instead of soft glow). We auto-generate the chain at
	// upload; textures authored without mips stay mip0 like GC.
	t->mip = (u16)(mipmap ? 1 : 0);
	// Content (re)defined -> the previous decode is invalid. Hand its GPU slot
	// back, or every per-frame re-init leaks one: the movie re-inits the same
	// two texobjs every frame and exhausted the pool in a few hundred frames,
	// after which uploads returned -1 and the video quad sampled white.
	// The release is deferred to end-of-frame inside the renderer, so draws
	// already recorded this frame keep the handle they were given.
	if (t->magic == PC_TEXOBJ_MAGIC && t->handle >= 0) {
		pc_window_release_texture(t->handle);
	}
	t->handle = -1;
	t->magic = PC_TEXOBJ_MAGIC;
}
void GXInitTexObjLOD(GXTexObj*, GXTexFilter, GXTexFilter, f32, f32, f32, GXBool, GXBool, GXAnisotropy) { }

unsigned long pc_gx_tex_nomagic_binds, pc_gx_tex_ok_binds;
// GX has 8 texmap slots; which one a draw samples is stage0's GXSetTevOrder
// choice, NOT the most recent bind (PVW materials bind maps 0..N up front
// and skip re-binding unchanged ones — "last bind wins" left curTex stale
// or on the wrong map; symptom: cinematic meshes untextured while simple
// bind-then-draw materials like the planet worked).
int pc_gx_texmap_handle(int m) { return (m >= 0 && m < 8) ? s_texmap[m] : -1; }
// curTex: >=0 bound handle; -1 stage-0 texmap is NULL/DISABLE (genuinely
// untextured — GX yields texel=0); -3 a texmap IS configured but this shim has
// no texture for it (a binding gap on our side — the program records a neutral
// white texel). -2 stays the skip sentinel set by GXLoadTexObj. Texture binds
// also change the recorded program (per-stage handles), so mark it dirty.
static void pc_update_curtex(void) {
	if (s_tev[0].texmap < 8)
		s_cap.curTex = (s_texmap[s_tev[0].texmap] == -1) ? -3 : s_texmap[s_tev[0].texmap];
	else
		s_cap.curTex = -1;
	s_tev_dirty = 1;
}
void GXLoadTexObj(GXTexObj* obj, GXTexMapID map) {
	int m = (int)map & 7;
	PCTexInfo* t = pc_texobj(obj);
	if (t->magic != PC_TEXOBJ_MAGIC || !t->image || t->w == 0 || t->h == 0) {
		pc_gx_tex_nomagic_binds++;
		s_texmap[m] = -1;
		pc_update_curtex();
		return;
	}
	pc_gx_tex_ok_binds++;
	// EFB-copy destination with a LIVE resolved texture (v35): bind it.
	{
		int ch = pc_window_copy_handle(t->image);
		if (ch >= 0) {
			t->handle = ch;
			s_texmap[m] = ch;
			pc_update_curtex();
			return;
		}
	}
	// -2 = a skip marker: an unpopulated screen-copy/grab texture (light map,
	// blur/glow source) that this deferred renderer never fills, so it decodes
	// all-black. Drawing it over the scene (shadow multiply, glow) would wipe
	// the frame; skip the draw instead and keep the real scene. GXCopyTex marks
	// its destinations; the all-black heuristic below catches grabs that never
	// even ran a copy.
	if (t->handle == -2 || pc_gx_is_copy_dst(t->image)) {
		s_texmap[m] = -2;
		pc_update_curtex();
		return;
	}
	// decoded handle rides inside the texobj; copies inherit it, re-init clears it
	if (t->handle >= 0) {
		s_texmap[m] = t->handle;
		pc_update_curtex();
		return;
	}
	static u32 buf[1024 * 1024];
	if ((int)t->w * (int)t->h > 1024 * 1024) { s_texmap[m] = -1; pc_update_curtex(); return; }
	// PC_DUMP_TEXHASH=<dir>: write each distinct texture's RAW (still tiled) GC
	// bytes plus its identity, so Dolphin-style texture-pack hashes can be
	// computed offline. Dolphin names packs tex1_<w>x<h>_<hash>_<fmt>.png and
	// hashes exactly these bytes, so this is the input side of matching a pack.
	if (const char* hd = getenv("PC_DUMP_TEXHASH")) {
		static const void* seen[512];
		static int nseen;
		bool have = false;
		for (int i = 0; i < nseen; i++) {
			if (seen[i] == t->image) { have = true; break; }
		}
		if (!have && nseen < 512) {
			seen[nseen++] = t->image;
			// GC tile geometry per format; every tile is 32 bytes except RGBA8.
			int tw = 8, th = 8, tb = 32;
			switch (t->fmt) {
			case 0x0: tw = 8; th = 8; break;              // I4
			case 0x1: case 0x2: tw = 8; th = 4; break;    // I8, IA4
			case 0x3: case 0x4: case 0x5: tw = 4; th = 4; break; // IA8, RGB565, RGB5A3
			case 0x6: tw = 4; th = 4; tb = 64; break;     // RGBA8
			case 0xE: tw = 8; th = 8; break;              // CMPR
			default: tw = 0; break;
			}
			if (tw) {
				const int txs = ((int)t->w + tw - 1) / tw, tys = ((int)t->h + th - 1) / th;
				const long bytes = (long)txs * tys * tb;
				char path[512];
				snprintf(path, sizeof(path), "%s/%ux%u_%u_%d.bin", hd, t->w, t->h, t->fmt, nseen - 1);
				FILE* f = fopen(path, "wb");
				if (f) {
					fwrite(t->image, 1, (size_t)bytes, f);
					fclose(f);
				}
				fprintf(stderr, "[TEXHASH] %ux%u fmt=%u bytes=%ld -> %s\n", t->w, t->h, t->fmt, bytes, path);
			}
		}
	}
	{
		const int hd = pc_hd_lookup((const u8*)t->image, (int)t->w, (int)t->h, (int)t->fmt, (int)t->wrapS,
		                            (int)t->wrapT, (int)t->mip);
		if (hd >= 0) {
			t->handle   = hd;
			s_texmap[m] = hd;
			pc_update_curtex();
			return;
		}
	}
	pc_decode_gc_tex((const u8*)t->image, t->w, t->h, t->fmt, buf);
	int npix = (int)t->w * (int)t->h, nz = 0;
	for (int i = 0; i < npix; i++)
		if (buf[i] & 0x00FFFFFF) nz++;
	// (Near-)black + large -> an unpopulated grab/screen-copy texture; skip draws.
	if ((long)nz * 100 < (long)npix && t->w >= 128 && t->h >= 128) {
		if (getenv("PC_VERBOSE"))
			fprintf(stderr, "[GXtex] %dx%d fmt=0x%x -> SKIP (unpopulated black copy, img=%p)\n", t->w, t->h, t->fmt, t->image);
		t->handle = -2;
		s_texmap[m] = -2;
		pc_update_curtex();
		return;
	}
	int handle = pc_window_upload_texture((const unsigned char*)buf, t->w, t->h, (int)t->wrapS, (int)t->wrapT, (int)t->mip);
	if (getenv("PC_VERBOSE"))
		fprintf(stderr, "[GXtex] %dx%d fmt=0x%x mip=%d -> handle %d (%d%% nonzero, img=%p)\n", t->w, t->h, t->fmt, (int)t->mip,
		        handle, npix ? nz * 100 / npix : 0, t->image);
	t->handle = handle;
	s_texmap[m] = handle;
	pc_update_curtex();
}

void GXSetNumTevStages(u8 n) {
	s_ntevstages = n;
	s_tev_dirty = 1;
}
void GXSetNumTexGens(u8) { }

// ---- TEV state recording -----------------------------------------------
// Every setter below writes the tracked stage state verbatim (values verified
// against GXEnum.h + the SDK's own GXTev.c) and marks the program snapshot
// dirty. NOTHING is interpreted here — pc_tev_snapshot() packages the state
// per draw segment and the Metal fragment shader evaluates the real GX stage
// equation: out = d [+/-] ((1-c)*a + c*b) + bias, * scale, clamp -> out_reg.
static float pc_clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
static float pc_clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Stage ids arrive straight from game code; a garbage one used to index
// s_tev[] out of bounds and segfault (P2DPicture::setTevMode on the card
// file-select screen). The setters below reject anything outside 0..7 via an
// UNSIGNED compare — negatives included — and report the first few so a bad
// id is diagnosed rather than silently swallowed.
static void pc_tev_bad_stage(int stage, const char* who) {
	static int n;
	if (n < 8) {
		n++;
		fprintf(stderr, "[TEVSTAGE] %s: out-of-range stage %d (0x%x) ignored\n", who, stage, (unsigned)stage);
	}
}

void GXSetTevOp(GXTevStageID stage, GXTevMode mode) {
	if ((unsigned)stage >= PC_TEV_MAXSTAGES) return pc_tev_bad_stage((int)stage, "GXSetTevOp"); // unsigned: also rejects negative/garbage ids
	// A GX_PASSCLR preset draw outputs the flat rasterized color from the vertex
	// (the default channel source). Genuine register-alpha fades (P2DPicture) set
	// GX_SRC_REG via GXSetChanCtrl and never take this preset, so clear any
	// STALE matsrc=REG left over from a prior draw — otherwise the register alpha
	// override forces PASSCLR draws (the glass shine/frame) opaque.
	if (mode == 4 /* GX_PASSCLR */ && stage == 0) s_chan0_matsrc = 1; // GX_SRC_VTX
	// Exact SDK expansion (src/gx/GXTev.c): stages > 0 use CPREV/APREV where
	// stage 0 uses RASC/RASA; both ops reset to ADD, no bias, scale 1, -> PREV.
	// GXTevMode (GXEnum.h): MODULATE=0 DECAL=1 BLEND=2 REPLACE=3 PASSCLR=4.
	int cras = (stage == 0) ? 10 : 0, aras = (stage == 0) ? 5 : 0;
	PcTevStage* S = &s_tev[stage];
	switch (mode) {
	case 1: // GX_DECAL: color = lerp(ras, tex, texA); alpha = ras
		S->ca = cras; S->cb = 8; S->cc = 9; S->cd = 15;
		S->aa = 7; S->ab = 7; S->ac = 7; S->ad = aras;
		break;
	case 2: // GX_BLEND: color = (1-tex)*ras + tex; alpha = texA * rasA
		S->ca = cras; S->cb = 12; S->cc = 8; S->cd = 15;
		S->aa = 7; S->ab = 4; S->ac = aras; S->ad = 7;
		break;
	case 3: // GX_REPLACE: out = tex (pure — GX truth, not tex*ras)
		S->ca = 15; S->cb = 15; S->cc = 15; S->cd = 8;
		S->aa = 7; S->ab = 7; S->ac = 7; S->ad = 4;
		break;
	case 4: // GX_PASSCLR: out = ras (or prev)
		S->ca = 15; S->cb = 15; S->cc = 15; S->cd = cras;
		S->aa = 7; S->ab = 7; S->ac = 7; S->ad = aras;
		break;
	default: // GX_MODULATE: out = tex * ras
		S->ca = 15; S->cb = 8; S->cc = cras; S->cd = 15;
		S->aa = 7; S->ab = 4; S->ac = aras; S->ad = 7;
		break;
	}
	S->cop = 0; S->cbias = 0; S->cscale = 0; S->cclamp = 1; S->cout = 0;
	S->aop = 0; S->abias = 0; S->ascale = 0; S->aclamp = 1; S->aout = 0;
	s_tev_dirty = 1;
}
void GXSetTevOrder(GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID chan) {
	if ((unsigned)stage >= PC_TEV_MAXSTAGES) return pc_tev_bad_stage((int)stage, "GXSetTevOrder"); // unsigned: also rejects negative/garbage ids
	int m = (int)map;
	PcTevStage* S = &s_tev[stage];
	S->texmap = (m >= 0 && m < 8) ? (u8)m : 0xFF; // GX_TEXMAP_NULL/DISABLE
	S->texcoord = ((int)coord >= 0 && (int)coord < 8) ? (u8)coord : 0xFF;
	S->chan = (u8)chan;
	s_tev_dirty = 1;
	if (stage == 0) pc_update_curtex();
}
void GXSetTevColor(GXTevRegID reg, GXColor c) {
	if (reg >= 1 && reg <= 3) {
		float* d = s_tev_regc[reg - 1];
		d[0] = c.r / 255.0f; d[1] = c.g / 255.0f; d[2] = c.b / 255.0f; d[3] = c.a / 255.0f;
		s_tev_dirty = 1;
	}
}
void GXSetTevColorS10(GXTevRegID reg, GXColorS10 c) {
	if (reg >= 1 && reg <= 3) {
		if (getenv("PC_TEV_TRACE")) {
			static u32 seen[128]; static int nseen;
			u32 key = ((u32)reg << 28) ^ ((u32)(u16)c.r << 16) ^ ((u32)(u16)c.g << 8) ^ (u32)(u16)c.b ^ ((u32)(u16)c.a << 4);
			bool found = false;
			for (int i = 0; i < nseen; i++) if (seen[i] == key) { found = true; break; }
			if (!found && nseen < 128) {
				seen[nseen++] = key;
				fprintf(stderr, "[TEVREG] S10 reg=%d rgba=(%d,%d,%d,%d)\n", (int)reg, c.r, c.g, c.b, c.a);
			}
		}
		// S10 means SIGNED 10-bit (-1024..1023), which is the entire reason this
		// entry point exists alongside GXSetTevColor — a TEV register is allowed
		// to hold a value outside [0,1] and is used to carry negative bias terms.
		// Clamping to [0,1] here threw those away.
		//
		// It showed up as the title-screen idle movie rendering pink. That draw
		// builds YUV->RGB out of four TEV stages and loads
		// GX_TEVREG0 = {-111, 0, -138, 68}: the negative red and blue terms are
		// the colour-difference biases. Clamped to zero, red and blue saturate
		// while green (carried through the ALPHA pipeline and moved across in
		// stage 3) survives — measured mean RGB (251, 127, 239), which is
		// precisely the pink that was on screen.
		//
		// The hardware range is +/-4 in TEV units (1023/255 ~ 4.01), and the
		// shader already clamps unclamped intermediates to [-4, 4], so this is
		// consistent with what the rest of the pipeline expects.
		float* d = s_tev_regc[reg - 1];
		d[0] = pc_clamp(c.r / 255.0f, -4.0f, 4.0f); d[1] = pc_clamp(c.g / 255.0f, -4.0f, 4.0f);
		d[2] = pc_clamp(c.b / 255.0f, -4.0f, 4.0f); d[3] = pc_clamp(c.a / 255.0f, -4.0f, 4.0f);
		s_tev_dirty = 1;
	}
}
// AUDIT CLOSED (v41): the VALUES arriving here are authentic and the
// hardware-interleaved interpretation below is CORRECT. The real GX SDK's
// GXTevColorArg is interleaved (C0=2, A0=3, C1=4, A1=5, C2=6) — it is this
// REPO's GXEnum.h whose NAMES are shifted (its "GX_CC_C2" carries value 4,
// the real C1). Because the decomp is asm-matched, game code passes the
// original raw values; e.g. setPrimEnv puts env in REG1 and particle
// tevMode 2 reads "GX_CC_C2"(=4)=real C1=REG1=env — a correct pairing.
// Do NOT remap values here (tried, blacked out the title, reverted);
// renaming the repo header's enumerators is a cosmetic decomp fix only.
void GXSetTevColorIn(GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d) {
	if ((unsigned)stage >= PC_TEV_MAXSTAGES) return pc_tev_bad_stage((int)stage, "GXSetTevColorIn"); // unsigned: also rejects negative/garbage ids
	PcTevStage* S = &s_tev[stage];
	S->ca = (u8)a; S->cb = (u8)b; S->cc = (u8)c; S->cd = (u8)d;
	s_tev_dirty = 1;
}
void GXSetTevAlphaIn(GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d) {
	if ((unsigned)stage >= PC_TEV_MAXSTAGES) return pc_tev_bad_stage((int)stage, "GXSetTevAlphaIn"); // unsigned: also rejects negative/garbage ids
	PcTevStage* S = &s_tev[stage];
	S->aa = (u8)a; S->ab = (u8)b; S->ac = (u8)c; S->ad = (u8)d;
	s_tev_dirty = 1;
}
void GXSetTevColorOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID out) {
	if ((unsigned)stage >= PC_TEV_MAXSTAGES) return pc_tev_bad_stage((int)stage, "GXSetTevColorOp"); // unsigned: also rejects negative/garbage ids
	PcTevStage* S = &s_tev[stage];
	S->cop = (u8)op; S->cbias = (u8)bias; S->cscale = (u8)scale;
	S->cclamp = clamp ? 1 : 0; S->cout = (u8)out & 3;
	s_tev_dirty = 1;
}
void GXSetTevAlphaOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID out) {
	if ((unsigned)stage >= PC_TEV_MAXSTAGES) return pc_tev_bad_stage((int)stage, "GXSetTevAlphaOp"); // unsigned: also rejects negative/garbage ids
	PcTevStage* S = &s_tev[stage];
	S->aop = (u8)op; S->abias = (u8)bias; S->ascale = (u8)scale;
	S->aclamp = clamp ? 1 : 0; S->aout = (u8)out & 3;
	s_tev_dirty = 1;
}
void GXSetTevKColor(GXTevKColorID id, GXColor c) {
	if ((int)id < 4) {
		float* d = s_tev_konstc[id];
		d[0] = c.r / 255.0f; d[1] = c.g / 255.0f; d[2] = c.b / 255.0f; d[3] = c.a / 255.0f;
		s_tev_dirty = 1;
	}
}
void GXSetTevKColorSel(GXTevStageID stage, GXTevKColorSel sel) {
	if ((unsigned)stage >= PC_TEV_MAXSTAGES) return pc_tev_bad_stage((int)stage, "GXSetTevKColorSel"); // unsigned: also rejects negative/garbage ids
	s_tev[stage].kcsel = (u8)sel;
	s_tev_dirty = 1;
}
void GXSetTevKAlphaSel(GXTevStageID stage, GXTevKAlphaSel sel) {
	if ((unsigned)stage >= PC_TEV_MAXSTAGES) return pc_tev_bad_stage((int)stage, "GXSetTevKAlphaSel"); // unsigned: also rejects negative/garbage ids
	s_tev[stage].kasel = (u8)sel;
	s_tev_dirty = 1;
}
// Swap tables (MODELED as of v43): per-stage swizzles applied to the texel and
// rasterized colour before the combiner. Pikmin drives its screen-wide
// desaturate/fade (gameCoreSection, mapMgr) and damage flash (damageEffect)
// through {R,R,R,A} tables + COMP_RGB8_GT stages — with these unmodeled those
// passes were silently identity.
void GXSetTevSwapMode(GXTevStageID stage, GXTevSwapSel rasSel, GXTevSwapSel texSel) {
	if ((unsigned)stage >= PC_TEV_MAXSTAGES) return pc_tev_bad_stage((int)stage, "GXSetTevSwapMode"); // unsigned: also rejects negative/garbage ids
	s_tev[stage].rasswap = (u8)((int)rasSel & 3);
	s_tev[stage].texswap = (u8)((int)texSel & 3);
	s_tev_dirty = 1;
}
void GXSetTevSwapModeTable(GXTevSwapSel sel, GXTevColorChan r, GXTevColorChan g, GXTevColorChan b, GXTevColorChan a) {
	int t = (int)sel & 3;
	s_swaptbl[t][0] = (u8)((int)r & 3);
	s_swaptbl[t][1] = (u8)((int)g & 3);
	s_swaptbl[t][2] = (u8)((int)b & 3);
	s_swaptbl[t][3] = (u8)((int)a & 3);
	s_tev_dirty = 1;
}
void GXSetTexCoordGen2(GXTexCoordID coord, GXTexGenType, GXTexGenSrc src, u32 mtxId, GXBool, u32) {
	if ((int)coord >= 0 && (int)coord < 8) {
		s_tcg_src[coord] = (int)src;
		s_tcg_mtx[coord] = (int)mtxId;
		s_tev_dirty = 1; // uvsel / sphere-need derive from the generators
	}
}

void GXSetAlphaCompare(GXCompare comp0, u8 ref0, GXAlphaOp, GXCompare, u8) {
	// Record the REAL comparator; the shader applies it exactly. Cutout
	// materials use GX_GEQUAL/0x80; particles GX_GEQUAL/1 (discard only
	// alpha=0) — the old fixed 0.5 discard amputated every soft additive
	// fringe (stars/glows read as hard sprites).
	s_atest_comp = (int)comp0 & 7;
	s_atest_ref = (int)ref0;
	s_atest = (comp0 != 7 /*GX_ALWAYS*/ && !(comp0 == 6 /*GEQUAL*/ && ref0 == 0)) ? 1 : 0;
	s_tev_dirty = 1;
}
void GXSetAlphaUpdate(GXBool) { }
void GXSetBlendMode(GXBlendMode type, GXBlendFactor src, GXBlendFactor dst, GXLogicOp lo) {
	// GXBlendFactor: 0 ZERO, 1 ONE, 2 SRCCOL, 3 INVSRCCOL, 4 SRCALPHA,
	// 5 INVSRCALPHA, 6 DSTALPHA, 7 INVDSTALPHA.
	if (getenv("PC_BLEND_TRACE")) {
		static u32 seen[64]; static int nseen;
		u32 key = ((u32)type << 16) | ((u32)(src & 0xff) << 8) | (u32)(dst & 0xff);
		bool found = false;
		for (int i = 0; i < nseen; i++) if (seen[i] == key) { found = true; break; }
		if (!found && nseen < 64) { seen[nseen++] = key; fprintf(stderr, "[BLEND] type=%d src=%d dst=%d logic=%d\n", type, src, dst, lo); }
	}
	// Pass the real GX factors through to the renderer, which maps them to the
	// exact Metal blend factors and builds a pipeline per config. Encoding:
	// 0 = opaque (no blend); else bit6=enabled | src<<3 | dst.
	if (type == 0 || type == 2) s_blend = 0;              // NONE / LOGIC -> opaque
	else s_blend = 0x40 | ((src & 7) << 3) | (dst & 7);   // BLEND / SUBTRACT (approx)
}
void GXSetColorUpdate(GXBool) { }
void GXSetZMode(GXBool enable, GXCompare, GXBool update) {
	s_zmode_enable = enable;
	s_zmode_write = update;
}
void GXSetZCompLoc(GXBool) { }
// Backface culling is REAL GX state the game depends on: translucent shells
// (number pellets, helmet domes) draw with z-write off and rely on GX_CULL_BACK
// to keep their inner faces invisible — a no-op here double-blended them
// (the "glass pellet" with the mirrored number showing through).
void GXSetCullMode(GXCullMode mode) { s_cull_mode = (int)mode & 3; }
void GXSetCoPlanar(GXBool) { }
void GXSetDither(GXBool) { }
void GXSetFog(GXFogType, f32, f32, f32, f32, GXColor) { }
void GXSetFogRangeAdj(GXBool, u16, GXFogAdjTable*) { }
// GX line width / point size are in 1/6-pixel units (the results-screen graph
// asks for 0x12 = 3px). This renderer only draws triangles, so the primitive
// assembler expands lines and points into screen-space quads at this width.
float pc_gx_line_width = 1.0f;
float pc_gx_point_size = 1.0f;
void GXSetLineWidth(u8 w, GXTexOffset) { pc_gx_line_width = w / 6.0f; }
void GXSetPointSize(u8 s, GXTexOffset) { pc_gx_point_size = s / 6.0f; }
void GXSetPixelFmt(GXPixelFmt, GXZFmt16) { }

void GXSetProjection(const Mtx44 mtx, GXProjectionType t) {
	memcpy(s_cap.proj, mtx, sizeof(f32) * 16);
	s_cap.projPersp = (t == 0);
}
void GXSetViewport(f32 x, f32 y, f32 w, f32 h, f32 nearZ, f32 farZ) {
	// Not yet applied to rendering — log distinct rects to learn whether the
	// game ever draws to a sub-viewport (would stretch under the no-op).
	static float seen[16][4];
	static int nseen;
	for (int i = 0; i < nseen; i++)
		if (seen[i][0] == x && seen[i][1] == y && seen[i][2] == w && seen[i][3] == h) return;
	if (nseen < 16) {
		seen[nseen][0] = x; seen[nseen][1] = y; seen[nseen][2] = w; seen[nseen][3] = h;
		nseen++;
		if (getenv("PC_VERBOSE"))
			fprintf(stderr, "[GXvp] viewport %g,%g %gx%g z=%g..%g\n", x, y, w, h, nearZ, farZ);
	}
}
void GXSetScissor(u32 x, u32 y, u32 w, u32 h) {
	static u32 seen[16][4];
	static int nseen;
	for (int i = 0; i < nseen; i++)
		if (seen[i][0] == x && seen[i][1] == y && seen[i][2] == w && seen[i][3] == h) return;
	if (nseen < 16) {
		seen[nseen][0] = x; seen[nseen][1] = y; seen[nseen][2] = w; seen[nseen][3] = h;
		nseen++;
		if (getenv("PC_VERBOSE")) fprintf(stderr, "[GXvp] scissor %u,%u %ux%u\n", x, y, w, h);
	}
}
void GXSetScissorBoxOffset(s32, s32) { }

void GXSetDispCopySrc(u16, u16, u16, u16) { }
void GXSetDispCopyDst(u16, u16) { }
u32 GXSetDispCopyYScale(f32) { return 0; }
void pc_window_set_clear(float r, float g, float b); // pc_window.mm
void GXSetCopyClear(GXColor c, u32) { pc_window_set_clear(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f); }
void GXSetCopyFilter(GXBool, const u8 (*)[2], GXBool, const u8*) { }
void GXCopyDisp(void*, GXBool) { }
static struct { u16 x, y, w, h; } s_texcopy_src;
void GXSetTexCopySrc(u16 x, u16 y, u16 w, u16 h) { s_texcopy_src.x = x; s_texcopy_src.y = y; s_texcopy_src.w = w; s_texcopy_src.h = h; }
static struct { u16 w, h, fmt; } s_texcopy_dst;
void GXSetTexCopyDst(u16 w, u16 h, GXTexFmt fmt, GXBool) { s_texcopy_dst.w = w; s_texcopy_dst.h = h; s_texcopy_dst.fmt = (u16)fmt; }

// GXCopyTex: EFB -> RAM texture (Texture::grabBuffer), the engine of Pikmin's
// full-screen post effects — the "infamous blur" feedback (MapMgr::postrefresh
// grabs the frame, blends it with LAST frame's blur, grabs the result), the
// warp-exit glow that buries the ray geometry, lightmaps. IMPLEMENTED (v35):
// pc_window records a copy EVENT at the current point in the deferred seg
// stream and resolves it at present time, in order — rendering the
// segs-so-far into a live Metal texture bound wherever the game samples the
// grab. PC_NO_EFB_COPY=1 reverts to the old skip-sentinel behavior.
static void* s_copy_dst[64];
static int s_ncopy_dst;
bool pc_gx_is_copy_dst(const void* p) {
	for (int i = 0; i < s_ncopy_dst; i++) if (s_copy_dst[i] == p) return true;
	return false;
}
void pc_window_copy_tex(const void* dest, int sx, int sy, int sw, int sh, int dw, int dh, int clear, int after_seg, int isDepth); // pc_window.mm
static int pc_efb_copy_on(void) {
	static int s = -1;
	if (s < 0) s = getenv("PC_NO_EFB_COPY") ? 0 : 1;
	return s;
}
void GXCopyTex(void* dest, GXBool clear) {
	static int s_copylog;
	if (getenv("PC_VERBOSE") && s_copylog < 4) {
		s_copylog++;
		fprintf(stderr, "[GXcopytex] dest=%p clear=%d src=%d,%d %dx%d dst=%dx%d\n", dest, clear,
		        s_texcopy_src.x, s_texcopy_src.y, s_texcopy_src.w, s_texcopy_src.h, s_texcopy_dst.w, s_texcopy_dst.h);
	}
	if (pc_efb_copy_on()) {
		// Anchor the copy at the STUB's current segment count: game draws
		// haven't flushed to the window yet mid-frame, so the window's own
		// counter is still 0 here (bug: every copy resolved before seg 0 —
		// an empty frame). Stub seg index == window seg index (the flush
		// replays segments 1:1, once per frame).
		// GX_TF_Z8 (0x11) dst = a DEPTH grab: Pikmin's "infamous blur" source
		// (mBlurSourceTexture) is Z8 — the 4-stage MultiTexture cascade uses
		// depth as the blend weight (a depth-of-field, not a full-screen
		// smear). Resolving COLOR into it saturates the weight to ~1 and
		// blurs the whole screen at full strength (user: "very heavy").
		pc_window_copy_tex(dest, s_texcopy_src.x, s_texcopy_src.y, s_texcopy_src.w, s_texcopy_src.h,
		                   s_texcopy_dst.w ? s_texcopy_dst.w : s_texcopy_src.w,
		                   s_texcopy_dst.h ? s_texcopy_dst.h : s_texcopy_src.h, (int)clear, s_nsegs,
		                   s_texcopy_dst.fmt == 0x11);
		return;
	}
	if (pc_gx_is_copy_dst(dest)) return;
	if (s_ncopy_dst < 64) s_copy_dst[s_ncopy_dst++] = dest;
}

// ------------------------------------------------------------------- MTX
// Real math — the game computes with these on the CPU.
void PSMTXIdentity(Mtx m) {
	memset(m, 0, sizeof(f32) * 12);
	m[0][0] = m[1][1] = m[2][2] = 1.0f;
}

void PSMTXConcat(const Mtx a, const Mtx b, Mtx dst) {
	Mtx t;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 4; j++)
			t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + (j == 3 ? a[i][3] : 0.0f);
	memcpy(dst, t, sizeof(t));
}

void PSMTXTranspose(const Mtx src, Mtx xPose) {
	Mtx t;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			t[i][j] = src[j][i];
	t[0][3] = t[1][3] = t[2][3] = 0.0f;
	memcpy(xPose, t, sizeof(t));
}

u32 PSMTXInverse(const Mtx src, Mtx inv) {
	f32 det = src[0][0] * (src[1][1] * src[2][2] - src[1][2] * src[2][1])
	        - src[0][1] * (src[1][0] * src[2][2] - src[1][2] * src[2][0])
	        + src[0][2] * (src[1][0] * src[2][1] - src[1][1] * src[2][0]);
	if (det == 0.0f) return 0;
	f32 r = 1.0f / det;
	Mtx t;
	t[0][0] = (src[1][1] * src[2][2] - src[1][2] * src[2][1]) * r;
	t[0][1] = -(src[0][1] * src[2][2] - src[0][2] * src[2][1]) * r;
	t[0][2] = (src[0][1] * src[1][2] - src[0][2] * src[1][1]) * r;
	t[1][0] = -(src[1][0] * src[2][2] - src[1][2] * src[2][0]) * r;
	t[1][1] = (src[0][0] * src[2][2] - src[0][2] * src[2][0]) * r;
	t[1][2] = -(src[0][0] * src[1][2] - src[0][2] * src[1][0]) * r;
	t[2][0] = (src[1][0] * src[2][1] - src[1][1] * src[2][0]) * r;
	t[2][1] = -(src[0][0] * src[2][1] - src[0][1] * src[2][0]) * r;
	t[2][2] = (src[0][0] * src[1][1] - src[0][1] * src[1][0]) * r;
	for (int i = 0; i < 3; i++)
		t[i][3] = -(t[i][0] * src[0][3] + t[i][1] * src[1][3] + t[i][2] * src[2][3]);
	memcpy(inv, t, sizeof(t));
	return 1;
}

void MTXScale(Mtx m, f32 x, f32 y, f32 z) {
	PSMTXIdentity(m);
	m[0][0] = x;
	m[1][1] = y;
	m[2][2] = z;
}

void MTXTrans(Mtx m, f32 x, f32 y, f32 z) {
	PSMTXIdentity(m);
	m[0][3] = x;
	m[1][3] = y;
	m[2][3] = z;
}

void MTXOrtho(Mtx44 m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f) {
	memset(m, 0, sizeof(f32) * 16);
	m[0][0] = 2.0f / (r - l);
	m[0][3] = -(r + l) / (r - l);
	m[1][1] = 2.0f / (t - b);
	m[1][3] = -(t + b) / (t - b);
	m[2][2] = -1.0f / (f - n);
	m[2][3] = -f / (f - n);
	m[3][3] = 1.0f;
}

void MTXPerspective(Mtx44 m, f32 fovY, f32 aspect, f32 n, f32 f) {
	memset(m, 0, sizeof(f32) * 16);
	f32 angle = fovY * 0.5f * 3.14159265358979f / 180.0f;
	f32 cot = __builtin_cosf(angle) / __builtin_sinf(angle);
	m[0][0] = cot / aspect;
	m[1][1] = cot;
	m[2][2] = -n / (f - n);
	m[2][3] = -(f * n) / (f - n);
	m[3][2] = -1.0f;
}

void MTXLightPerspective(Mtx m, f32 fovY, f32 aspect, f32 scaleS, f32 scaleT, f32 transS, f32 transT) {
	f32 angle = fovY * 0.5f * 3.14159265358979f / 180.0f;
	f32 cot = __builtin_cosf(angle) / __builtin_sinf(angle);
	memset(m, 0, sizeof(f32) * 12);
	m[0][0] = (cot / aspect) * scaleS;
	m[0][2] = -transS;
	m[1][1] = cot * scaleT;
	m[1][2] = -transT;
	m[2][2] = -1.0f;
}

f32 VECMag(const Vec* v) { return __builtin_sqrtf(v->x * v->x + v->y * v->y + v->z * v->z); }

} // extern "C"
