// pc_window.mm — M3 stage 1: the glass. SDL window + CAMetalLayer,
// presented once per VI retrace (pc_sdk_shims.cpp calls pc_window_frame()).
// Stage 2 hands the drawable to the real GX renderer; for now it clears —
// proving windowing, pacing, and event flow before any geometry.

#include <stdio.h>
#include <stdlib.h>

#include <TargetConditionals.h> // TARGET_OS_IPHONE — the iOS lifecycle handling

#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <SDL.h>
#include <string.h>

#include "pc_config.h"
#include "pc_sync.h"
#include "pc_touch_draw.h"
#include "pc_touch_pad.h"
#include "pc_tev_prog.h"

extern "C" {
void pc_window_frame(void);
void pc_window_set_clear(float r, float g, float b);
extern "C" unsigned int pc_input_current_tick(void); // pc_input_rec.cpp
void pc_window_get_pad(unsigned short* buttons, signed char* stickX, signed char* stickY, signed char* subX,
                       signed char* subY, unsigned char* trigL, unsigned char* trigR);
void pc_window_submit_tris(const float* verts, int vertCount, int tex, int depth, const PcTevProg* prog);
int pc_window_upload_texture(const unsigned char* rgba, int w, int h, int wrapS, int wrapT, int mip);
int pc_is_main_thread(void); // pc_sdk_shims.cpp
}

// ---- Gamepad ------------------------------------------------------------
// SDL's GameController layer hands us an already-normalised pad (it ships
// DualSense/Xbox mappings), so nothing here is device-specific. State is
// POLLED once per PADRead alongside the keyboard rather than accumulated
// from events: this is a fixed-timestep 30fps game where frame rate IS game
// speed, so input must be sampled on the game's tick, not between ticks.
// Keyboard and pad are OR-ed together, so both stay live at once.
// PC_NO_PAD=1 ignores controllers entirely.
static SDL_GameController* s_pad;
static SDL_JoystickID s_pad_id = -1;

static int pc_pad_disabled(void) {
	// input.gamepad in settings.ini; PC_NO_PAD still overrides (see the env
	// table in pc_config.cpp).
	static int v = -1;
	if (v < 0) v = pc_config_int("input.gamepad", 1) ? 0 : 1;
	return v;
}

// ---- input mapping + press latch ----------------------------------------
// Input is SAMPLED once per frame (PADRead runs on the game's 30fps tick and
// the only SDL pump is in pc_window_frame), so testing the instantaneous
// down-state alone drops any press that begins and ends between two samples
// — a ~33ms blind window, which is why quick pad taps needed spamming.
// Fix: the event loop LATCHES button-down events into s_input_latch, and the
// next PADRead ORs that in and clears it. A 5ms tap therefore reads as held
// for exactly one frame, which is what Controller::updateCont's rising-edge
// test needs. Held buttons are covered by the level read as before, so a
// latched bit can at worst extend a press by one frame — never add an edge.
static unsigned short s_input_latch;

// GC button bit for an SDL controller button (0 = unmapped).
static unsigned short pc_pad_bit(int button) {
	switch (button) {
	case SDL_CONTROLLER_BUTTON_A: return 0x0100;
	case SDL_CONTROLLER_BUTTON_B: return 0x0200;
	case SDL_CONTROLLER_BUTTON_X: return 0x0400;
	case SDL_CONTROLLER_BUTTON_Y: return 0x0800;
	case SDL_CONTROLLER_BUTTON_START: return 0x1000;
	case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return 0x0010;  // Z
	case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return 0x0020; // R
	case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return 0x0001;
	case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return 0x0002;
	case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return 0x0004;
	case SDL_CONTROLLER_BUTTON_DPAD_UP: return 0x0008;
	default: return 0;
	}
}

// Keyboard bindings, shared by the level read and the latch so the two can
// never drift apart. Several scancodes may map to one GC button.
//
// M7: the table is now BUILT FROM pc_config (keybindings.ini), with the
// compiled-in defaults living in pc_config.cpp. Previously the button map and
// the stick keys were two separate hardcoded lists in this file, which is
// exactly the arrangement that lets them disagree.
#define PC_MAX_BINDS 64
static struct {
	int scancode;
	unsigned short bit;
} s_key_map[PC_MAX_BINDS];
static int s_nkey_map;

// Stick direction scancodes, likewise from the config.
static int s_stick_keys[4][4]; // [up,down,left,right][scancodes]
static int s_stick_n[4];
static int s_cstick_keys[4][4];
static int s_cstick_n[4];

static void pc_bind_button(const char* action, unsigned short bit) {
	int sc[4];
	const int n = pc_config_keys(action, sc, 4);
	for (int i = 0; i < n && s_nkey_map < PC_MAX_BINDS; i++) {
		s_key_map[s_nkey_map].scancode = sc[i];
		s_key_map[s_nkey_map].bit      = bit;
		s_nkey_map++;
	}
}

static void pc_bindings_init(void) {
	static int done;
	if (done) return;
	done = 1;

	pc_bind_button("start", 0x1000);
	pc_bind_button("a", 0x0100);
	pc_bind_button("b", 0x0200);
	pc_bind_button("x", 0x0400);
	pc_bind_button("y", 0x0800);
	pc_bind_button("z", 0x0010);
	pc_bind_button("l", 0x0040);
	pc_bind_button("r", 0x0020);
	pc_bind_button("dleft", 0x0001);
	pc_bind_button("dright", 0x0002);
	pc_bind_button("ddown", 0x0004);
	pc_bind_button("dup", 0x0008);

    static const char* stick[4]  = { "up", "down", "left", "right" };
    static const char* cstick[4] = { "cup", "cdown", "cleft", "cright" };
	for (int i = 0; i < 4; i++) {
		s_stick_n[i]  = pc_config_keys(stick[i], s_stick_keys[i], 4);
		s_cstick_n[i] = pc_config_keys(cstick[i], s_cstick_keys[i], 4);
	}
}

// True if any scancode bound to this direction is down.
static int pc_dir_down(const Uint8* k, const int* keys, int n) {
	for (int i = 0; i < n; i++)
		if (k[keys[i]]) return 1;
	return 0;
}

static unsigned short pc_key_bit(int scancode) {
	pc_bindings_init();
	for (int i = 0; i < s_nkey_map; i++)
		if (s_key_map[i].scancode == scancode) return s_key_map[i].bit;
	return 0;
}

static void pc_pad_open_first(void) {
	if (pc_pad_disabled() || s_pad) return;
	for (int i = 0; i < SDL_NumJoysticks(); i++) {
		if (!SDL_IsGameController(i)) continue;
		SDL_GameController* c = SDL_GameControllerOpen(i);
		if (!c) continue;
		s_pad    = c;
		s_pad_id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c));
		fprintf(stderr, "[PC] gamepad: %s\n", SDL_GameControllerName(c) ? SDL_GameControllerName(c) : "(unnamed)");
		return;
	}
}

static void pc_pad_closed(SDL_JoystickID which) {
	if (s_pad && which == s_pad_id) {
		SDL_GameControllerClose(s_pad);
		s_pad    = nullptr;
		s_pad_id = -1;
		fprintf(stderr, "[PC] gamepad disconnected\n");
		pc_pad_open_first(); // fall back to another pad if one is still attached
	}
}

static SDL_Window* s_window;
static CAMetalLayer* s_layer;
static id<MTLDevice> s_device;
static id<MTLCommandQueue> s_queue;
// [atest][blendcode]. blendcode 0 = opaque; else bit6=enabled, bits3-5 = GX
// src factor, bits0-2 = GX dst factor. Built lazily. One fragment function
// (fmain_tev) evaluates every recorded TEV program.
static id<MTLRenderPipelineState> s_psos[2][128];
static id<MTLFunction> s_vfn;
static id<MTLLibrary> s_lib;
static id<MTLDepthStencilState> s_ds[4]; // bit0 test, bit1 write
static id<MTLTexture> s_depth_tex;
static id<MTLBuffer> s_vbuf[3]; // ring: setVertexBytes caps at 4KB
static int s_vbuf_idx;
static bool s_init_failed;

// Frame triangle list: 16 floats per vertex (clip xyzw + rgba + st + st2 +
// addc), segmented by texture; filled during the game frame, drawn at present.
//
// Capacity comes from pc_tev_prog.h, shared with the producer. These used to be
// private literals here (262144 / 2048); pc_gx_stub.cpp raised its own copies in
// v93/v94 and these did not move, so the shim captured frames that this side
// then threw the tail of away. See pc_tev_prog.h for the measurements.
#define PC_MAX_FRAME_VERTS PC_CAP_MAX_VERTS
#define PC_MAX_SEGS PC_CAP_MAX_SEGS
#define PC_MAX_TEXTURES 512
static float s_tris[PC_MAX_FRAME_VERTS * 16];
static int s_tri_verts;
static double s_clear[3] = { 0.05, 0.15, 0.08 };
extern "C" void pc_window_set_clear(float r, float g, float b) {
	s_clear[0] = r; s_clear[1] = g; s_clear[2] = b;
}
static struct { int tex, start, count, depth; PcTevProg prog; } s_draw_segs[PC_MAX_SEGS];
static int s_draw_nsegs;
static int s_diag_rejects;  // PC_DIAG_DRAW: submits dropped by the frame-vert/seg cap
static int s_reject_warned; // the cap has been reported once, loudly
static int s_want_verts;    // verts this frame ASKED for, uncapped
static int s_want_segs;     // segments this frame ASKED for, uncapped
static int s_drawable_miss; // frames dropped for want of a drawable (see pc_window_frame)
static id<MTLTexture> s_textures[PC_MAX_TEXTURES];
static float s_tex_mean[PC_MAX_TEXTURES]; // PC_DIAG_SEG: decoded brightness per handle
static id<MTLSamplerState> s_tex_samplers[PC_MAX_TEXTURES];
static int s_ntextures;

// TEXTURE SLOT RECYCLING. s_ntextures used to only ever climb, and nothing
// released a slot — so any texture re-uploaded every frame leaked one slot per
// frame. The title-screen movie uploads TWO 640x480 textures per frame (Y and
// UV; GXInitTexObj invalidates the cached decode each frame by design), so it
// exhausted all PC_MAX_TEXTURES in a few hundred frames. After that
// pc_window_upload_texture returned -1, nothing bound, the shader sampled
// white, and the movie's YUV->RGB TEV turned that into flat pink — the
// "plays for a second then sticks on a magenta frame" symptom. It never
// crashed because running out is a clean -1.
//
// Frees are DEFERRED to the end of the frame: draw segments recorded earlier in
// this frame still reference their handle, so recycling a slot immediately
// would let a later upload land under an earlier draw.
static int s_tex_free[PC_MAX_TEXTURES];
static int s_ntex_free;
static int s_tex_pending[PC_MAX_TEXTURES];
static int s_ntex_pending;

extern "C" void pc_window_release_texture(int handle) {
	if (handle < 0 || handle >= PC_MAX_TEXTURES || !s_textures[handle]) return;
	for (int i = 0; i < s_ntex_pending; i++)
		if (s_tex_pending[i] == handle) return; // already queued
	if (s_ntex_pending < PC_MAX_TEXTURES) s_tex_pending[s_ntex_pending++] = handle;
}

// Called once per frame, after the command buffer is committed. Metal retains
// textures referenced by in-flight command buffers, so dropping our reference
// here is safe.
static void pc_tex_drain_frees(void) {
	for (int i = 0; i < s_ntex_pending; i++) {
		const int h = s_tex_pending[i];
		if (h < 0 || h >= PC_MAX_TEXTURES || !s_textures[h]) continue;
		s_textures[h] = nil;
		s_tex_samplers[h] = nil;
		if (s_ntex_free < PC_MAX_TEXTURES) s_tex_free[s_ntex_free++] = h;
	}
	s_ntex_pending = 0;
}
static id<MTLSamplerState> s_sampler_cache[3][3][2];
static id<MTLTexture> s_efb_tex;                 // PC_EFB_LOWRES scene target
static id<MTLRenderPipelineState> s_ups_pso;     // low-res -> window upscale

// EFB-copy (GXCopyTex) support: each grab destination pointer owns a live
// Metal texture (registered in s_textures so segment binds work unchanged).
// A copy EVENT marks where in the deferred seg stream the grab happened; the
// present loop splits the scene encoder there and renders the scene-so-far
// into the copy texture, in stream order.
#define PC_MAX_COPIES 16
#define PC_MAX_COPY_EVENTS 32
static struct { const void* dest; int handle, w, h; } s_copies[PC_MAX_COPIES];
static int s_ncopies;
static struct { int after_seg, slot, sx, sy, sw, sh, clear, isDepth; } s_copy_events[PC_MAX_COPY_EVENTS];
static int s_ncopy_events;
static id<MTLRenderPipelineState> s_copy_pso;  // scene colour -> copy texture (subrect)
static id<MTLRenderPipelineState> s_copyz_pso; // scene DEPTH -> copy texture (Z8 grabs)
static id<MTLSamplerState> s_depth_sampler;    // nearest/clamp (depth32 isn't filterable)
static id<MTLSamplerState> pc_sampler(int ws, int wt, int mip); // defined below

extern "C" int pc_window_copy_handle(const void* dest) {
	for (int i = 0; i < s_ncopies; i++)
		if (s_copies[i].dest == dest) return s_copies[i].handle;
	return -1;
}
extern "C" void pc_window_copy_tex(const void* dest, int sx, int sy, int sw, int sh, int dw, int dh, int clear, int after_seg, int isDepth) {
	if (!s_device) return;
	int slot = -1;
	for (int i = 0; i < s_ncopies; i++)
		if (s_copies[i].dest == dest) { slot = i; break; }
	if (slot < 0) {
		// Same rule as the draw-stream caps: a capacity clamp that changes what is
		// drawn says so. Not raised — nothing has measured these being hit, and
		// v95's lesson is that guessing at a cap is how you fix the wrong one.
		// This just makes a hit visible if it ever happens.
		if (s_ncopies >= PC_MAX_COPIES || s_ntextures >= PC_MAX_TEXTURES) {
			static int warned = 0;
			if (!warned) {
				warned = 1;
				fprintf(stderr, "[PC] *** EFB copy destinations FULL (%d copies / %d textures) - this grab gets no "
				                "texture and its post effect will not draw. Raise PC_MAX_COPIES.\n",
				        PC_MAX_COPIES, PC_MAX_TEXTURES);
			}
			return;
		}
		if (dw <= 0 || dh <= 0) return;
		slot = s_ncopies++;
		MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
		                                                                              width:(NSUInteger)dw
		                                                                             height:(NSUInteger)dh
		                                                                          mipmapped:NO];
		td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		td.storageMode = MTLStorageModePrivate;
		s_textures[s_ntextures] = [s_device newTextureWithDescriptor:td];
		s_tex_samplers[s_ntextures] = pc_sampler(0, 0, 0);
		s_tex_mean[s_ntextures] = 128.0f; // live content; never "all black"
		s_copies[slot].dest = dest;
		s_copies[slot].handle = s_ntextures++;
		s_copies[slot].w = dw;
		s_copies[slot].h = dh;
	}
	if (s_ncopy_events < PC_MAX_COPY_EVENTS) {
		// Caller (the GX stub) supplies its mid-frame segment count; the
		// window's s_draw_nsegs is 0 until the end-of-frame flush.
		s_copy_events[s_ncopy_events].after_seg = after_seg;
		s_copy_events[s_ncopy_events].slot = slot;
		s_copy_events[s_ncopy_events].sx = sx;
		s_copy_events[s_ncopy_events].sy = sy;
		s_copy_events[s_ncopy_events].sw = sw;
		s_copy_events[s_ncopy_events].sh = sh;
		s_copy_events[s_ncopy_events].clear = clear;
		s_copy_events[s_ncopy_events].isDepth = isDepth;
		s_ncopy_events++;
	} else {
		// Dropping a copy EVENT is the flicker-shaped one: the grab keeps LAST
		// frame's contents, so the effect sampling it changes between frames.
		static int warned = 0;
		if (!warned) {
			warned = 1;
			fprintf(stderr, "[PC] *** EFB copy events FULL (%d this frame) - this grab keeps the PREVIOUS frame's "
			                "contents and its effect will flicker. Raise PC_MAX_COPY_EVENTS.\n",
			        PC_MAX_COPY_EVENTS);
		}
	}
}

static MTLSamplerAddressMode pc_addr(int gxWrap) {
	switch (gxWrap) {
	case 1: return MTLSamplerAddressModeRepeat;
	case 2: return MTLSamplerAddressModeMirrorRepeat;
	default: return MTLSamplerAddressModeClampToEdge;
	}
}
static id<MTLSamplerState> pc_sampler(int ws, int wt, int mip) {
	ws %= 3; wt %= 3; mip = mip ? 1 : 0;
	if (!s_sampler_cache[ws][wt][mip]) {
		MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
		sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
		// GC textures authored with a mip chain sample GX_LIN_MIP_LIN
		// (trilinear) — see GXInitTexObjLOD in texture.cpp.
		if (mip) sd.mipFilter = MTLSamplerMipFilterLinear;
		sd.sAddressMode = pc_addr(ws);
		sd.tAddressMode = pc_addr(wt);
		s_sampler_cache[ws][wt][mip] = [s_device newSamplerStateWithDescriptor:sd];
	}
	return s_sampler_cache[ws][wt][mip];
}

// fmain_tev declares array<texture2d,8> / array<sampler,8>, and a draw only
// binds the slots its TEV program actually samples — anything from a
// single-stage draw upward leaves the rest of the array unbound. The GPU never
// reads them (the shader gates on the per-stage tex flag), so this ran for
// months, but Metal API VALIDATION checks bindings statically and aborts on the
// first draw. It is off when the binary is launched from a shell and ON by
// default for an Xcode scheme, which is why this only ever fires on device.
//
// Binding a 1x1 white texture into all eight slots when the encoder opens
// satisfies it. Encoder state persists across draws, so this is once per
// encoder, not once per draw, and the real bindings simply overwrite the slots
// they use. White (not black) so that if a stage is ever mis-flagged as
// sampling, the texel is the TEV identity rather than a black-out.
static id<MTLTexture> s_tev_dummy;

static void pc_bind_tev_defaults(id<MTLRenderCommandEncoder> enc)
{
	if (!s_tev_dummy) {
		MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
		                                                                             width:1
		                                                                            height:1
		                                                                         mipmapped:NO];
		td.usage       = MTLTextureUsageShaderRead;
		td.storageMode = MTLStorageModeShared;
		s_tev_dummy    = [s_device newTextureWithDescriptor:td];
		const unsigned char white[4] = { 255, 255, 255, 255 };
		[s_tev_dummy replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:white bytesPerRow:4];
	}
	id<MTLSamplerState> smp = pc_sampler(0, 0, 0);
	for (NSUInteger i = 0; i < 8; i++) {
		[enc setFragmentTexture:s_tev_dummy atIndex:i];
		[enc setFragmentSamplerState:smp atIndex:i];
	}
}

// GXBlendFactor -> MTLBlendFactor. GX reuses the SRCCOL/INVSRCCOL enums (2,3):
// as a *source* factor they mean dest colour; as a *dest* factor, source colour.
static MTLBlendFactor pc_mtl_bf(int f, bool isSrc) {
	switch (f) {
	case 0: return MTLBlendFactorZero;
	case 1: return MTLBlendFactorOne;
	case 2: return isSrc ? MTLBlendFactorDestinationColor : MTLBlendFactorSourceColor;
	case 3: return isSrc ? MTLBlendFactorOneMinusDestinationColor : MTLBlendFactorOneMinusSourceColor;
	case 4: return MTLBlendFactorSourceAlpha;
	case 5: return MTLBlendFactorOneMinusSourceAlpha;
	case 6: return MTLBlendFactorDestinationAlpha;
	case 7: return MTLBlendFactorOneMinusDestinationAlpha;
	default: return isSrc ? MTLBlendFactorOne : MTLBlendFactorZero;
	}
}

// Lazily build a pipeline for (blendcode, alpha-test). blendcode 0 = opaque;
// else bit6=enabled, bits3-5 = GX src factor, bits0-2 = GX dst factor.
static id<MTLRenderPipelineState> pc_get_pso(int blendcode, int atest) {
	blendcode &= 0x7f;
	int idx = atest ? 1 : 0;
	if (s_psos[idx][blendcode]) return s_psos[idx][blendcode];
	MTLFunctionConstantValues* cv = [MTLFunctionConstantValues new];
	bool at = atest != 0;
	[cv setConstantValue:&at type:MTLDataTypeBool atIndex:0];
	NSError* ferr = nil;
	id<MTLFunction> ffn = [s_lib newFunctionWithName:@"fmain_tev" constantValues:cv error:&ferr];
	MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = s_vfn;
	pd.fragmentFunction = ffn;
	pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
	if (blendcode & 0x40) { // blending enabled
		int src = (blendcode >> 3) & 7, dst = blendcode & 7;
		pd.colorAttachments[0].blendingEnabled = YES;
		pd.colorAttachments[0].sourceRGBBlendFactor = pc_mtl_bf(src, true);
		pd.colorAttachments[0].destinationRGBBlendFactor = pc_mtl_bf(dst, false);
		// Keep framebuffer alpha at 1.0 (display target) regardless of colour blend.
		pd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorZero;
		pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
	}
	NSError* err = nil;
	s_psos[idx][blendcode] = [s_device newRenderPipelineStateWithDescriptor:pd error:&err];
	if (!s_psos[idx][blendcode])
		fprintf(stderr, "[PC] pso failed (blend 0x%x atest %d): %s\n", blendcode, atest, [[err localizedDescription] UTF8String]);
	return s_psos[idx][blendcode];
}

// ---- widescreen -------------------------------------------------------------
// Justin, 2026-11-25: "make it widescreen like we did with zelda 64". The game's
// cameras widen themselves (patches/0005: CullFrustum::update multiplies a 4:3
// aspect by pc_widescreen_scale), so 3D needs nothing here but the full-width
// viewport. 2D batches (depth-word bit 12, ortho projection) are narrowed back to
// 4:3 and centred so the HUD keeps its shape, unless the batch spans the whole
// screen (fades, full-screen backdrops), which is stretched to fill. Segments that
// feed a SUB-RECT EFB copy (shadow and effect textures drawn into a corner of the
// EFB) keep the old 4:3 viewport, so the copy still samples what it expects.
// On by default on iPhone/iPad; PC_WIDESCREEN=0 turns it off, =1 forces it on Mac.
static float s_ws_scale = 1.0f; // target aspect / (4/3), 1 when off or not wider
static int pc_widescreen_enabled(void) {
	static int on = -1;
	if (on < 0) {
		const char* e = getenv("PC_WIDESCREEN");
#if TARGET_OS_IPHONE
		on = e ? (atoi(e) != 0) : 1;
#else
		on = e ? (atoi(e) != 0) : 0;
#endif
	}
	return on;
}
extern "C" float pc_widescreen_scale(void) { return s_ws_scale; }

extern "C" void pc_window_submit_tris(const float* verts, int vertCount, int tex, int depth, const PcTevProg* prog) {
	// Report the UNCAPPED demand: a pinned counter reads the same whether the
	// frame wanted one vert more than capacity or ten times it.
	s_want_verts += vertCount;
	s_want_segs++;
	if (s_tri_verts + vertCount > PC_MAX_FRAME_VERTS || s_draw_nsegs >= PC_MAX_SEGS) {
		if (getenv("PC_DIAG_DRAW")) s_diag_rejects++;
		// Loud, once. Dropping a segment on its way to the GPU is a visible bug,
		// not a benign clamp — and hiding it behind a PC_DIAG_DRAW-only counter
		// is exactly why it outlived two rounds of flicker fixes.
		if (!s_reject_warned) {
			s_reject_warned = 1;
			fprintf(stderr, "[PC] *** draw stream FULL (%d verts / %d segs) - dropping whole segments. "
			                "Geometry and UI will flicker. This side must match pc_gx_stub.cpp's capture.\n",
			        PC_MAX_FRAME_VERTS, PC_MAX_SEGS);
		}
		return;
	}
	s_draw_segs[s_draw_nsegs].depth = depth;
	s_draw_segs[s_draw_nsegs].tex = tex;
	s_draw_segs[s_draw_nsegs].start = s_tri_verts;
	s_draw_segs[s_draw_nsegs].count = vertCount;
	memcpy(&s_draw_segs[s_draw_nsegs].prog, prog, sizeof(PcTevProg));
	s_draw_nsegs++;
	memcpy(&s_tris[s_tri_verts * 16], verts, (size_t)vertCount * 16 * sizeof(float));
	s_tri_verts += vertCount;
}

// Upload a BC7 .dds from disk (HD texture packs — see docs/ARCHITECTURE.md).
// The GPI pack is 2020 files, every one DX10/DXGI 98 = BC7_UNORM, so only that
// combination is handled; anything else returns -1 and the caller falls back to
// the game's own texture. BC formats are supported on Apple silicon but not
// unconditionally, hence the runtime capability check.
extern "C" int pc_window_upload_dds(const char* path, int wrapS, int wrapT) {
	if (!s_device || !path) return -1;
	static int bc_ok = -1;
	if (bc_ok < 0) {
		bc_ok = 0;
		if (@available(macOS 11.0, *)) bc_ok = s_device.supportsBCTextureCompression ? 1 : 0;
		if (!bc_ok) fprintf(stderr, "[HD] this GPU reports no BC texture support — DDS packs disabled\n");
	}
	if (!bc_ok) return -1;

	FILE* f = fopen(path, "rb");
	if (!f) return -1;
	unsigned char hdr[148];
	if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) || memcmp(hdr, "DDS ", 4) != 0) { fclose(f); return -1; }

	auto rd32 = [&](int off) { return (unsigned)hdr[off] | ((unsigned)hdr[off+1] << 8)
	                                  | ((unsigned)hdr[off+2] << 16) | ((unsigned)hdr[off+3] << 24); };
	const int h = (int)rd32(12), w = (int)rd32(16);
	const bool dx10 = memcmp(hdr + 84, "DX10", 4) == 0;
	const unsigned dxgi = dx10 ? rd32(128) : 0;
	if (!dx10 || dxgi != 98 /* BC7_UNORM */ || w <= 0 || h <= 0 || w > 8192 || h > 8192) { fclose(f); return -1; }

	// BC7: 16 bytes per 4x4 block.
	const size_t rowBlocks = (size_t)((w + 3) / 4), colBlocks = (size_t)((h + 3) / 4);
	const size_t bytesPerRow = rowBlocks * 16, total = bytesPerRow * colBlocks;
	unsigned char* px = (unsigned char*)malloc(total);
	if (!px) { fclose(f); return -1; }
	const size_t got = fread(px, 1, total, f);
	fclose(f);
	if (got != total) { free(px); return -1; }

	MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBC7_RGBAUnorm
	                                                                              width:(NSUInteger)w
	                                                                             height:(NSUInteger)h
	                                                                          mipmapped:NO];
	id<MTLTexture> tex = [s_device newTextureWithDescriptor:td];
	if (!tex) { free(px); return -1; }
	[tex replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h)
	       mipmapLevel:0
	         withBytes:px
	       bytesPerRow:(NSUInteger)bytesPerRow];
	free(px);

	int slot = -1;
	if (s_ntex_free > 0) slot = s_tex_free[--s_ntex_free];
	else if (s_ntextures < PC_MAX_TEXTURES) slot = s_ntextures++;
	else return -1;
	s_textures[slot] = tex;
	s_tex_samplers[slot] = pc_sampler(wrapS, wrapT, 0);
	s_tex_mean[slot] = 128.0f;
	return slot;
}

// Upload a PNG from disk as a texture (HD texture packs — see docs/ARCHITECTURE.md).
// ImageIO is already linked for the frame dumps, so this needs no new dependency.
// Returns -1 on any failure, and every caller treats that as "use the original",
// so a broken or missing PNG degrades to stock rather than to a visual glitch.
extern "C" int pc_window_upload_png(const char* path, int wrapS, int wrapT, int mip) {
	if (!s_device || !path) return -1;
	NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
	CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
	if (!src) return -1;
	CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
	CFRelease(src);
	if (!img) return -1;
	const int w = (int)CGImageGetWidth(img), h = (int)CGImageGetHeight(img);
	if (w <= 0 || h <= 0 || (long)w * h > 4096L * 4096L) { CGImageRelease(img); return -1; }

	unsigned char* rgba = (unsigned char*)calloc((size_t)w * h, 4);
	if (!rgba) { CGImageRelease(img); return -1; }
	CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
	CGContextRef ctx   = CGBitmapContextCreate(rgba, w, h, 8, (size_t)w * 4, cs,
	                                           kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
	if (ctx) {
		CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
		CGContextRelease(ctx);
	}
	CGColorSpaceRelease(cs);
	CGImageRelease(img);
	const int handle = ctx ? pc_window_upload_texture(rgba, w, h, wrapS, wrapT, mip) : -1;
	free(rgba);
	return handle;
}

extern "C" int pc_window_upload_texture(const unsigned char* rgba, int w, int h, int wrapS, int wrapT, int mip) {
	if (!s_device) return -1;
	int slot = -1;
	if (s_ntex_free > 0) {
		slot = s_tex_free[--s_ntex_free]; // recycle before growing
	} else if (s_ntextures < PC_MAX_TEXTURES) {
		slot = s_ntextures++;
	} else {
		static bool warned;
		if (!warned) {
			warned = true;
			fprintf(stderr, "[PC] texture pool exhausted (%d live) — draws will bind nothing\n", PC_MAX_TEXTURES);
		}
		return -1;
	}
	if (getenv("PC_DUMP_TEX")) {
		// Dump decoded RGBA to PNG (tex_<handle>.png) + report mean brightness.
		double sum = 0; for (long i = 0; i < (long)w * h; i++) sum += rgba[i*4] + rgba[i*4+1] + rgba[i*4+2];
		double mean = sum / ((double)w * h * 3.0);
		fprintf(stderr, "[TEXDUMP] handle %d  %dx%d  mean=%.1f\n", slot, w, h, mean);
		char path[64]; snprintf(path, sizeof(path), "tex_%03d.png", slot);
		CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
		CGContextRef ctx = CGBitmapContextCreate((void*)rgba, w, h, 8, w * 4, cs,
		                                         kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
		CGImageRef img = CGBitmapContextCreateImage(ctx);
		NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
		CGImageDestinationRef dst = CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, nullptr);
		if (dst) { CGImageDestinationAddImage(dst, img, nullptr); CGImageDestinationFinalize(dst); CFRelease(dst); }
		CGImageRelease(img); CGContextRelease(ctx); CGColorSpaceRelease(cs);
	}
	MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
	                                                                              width:(NSUInteger)w
	                                                                             height:(NSUInteger)h
	                                                                          mipmapped:(mip ? YES : NO)];
	id<MTLTexture> tex = [s_device newTextureWithDescriptor:td];
	[tex replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h)
	       mipmapLevel:0
	         withBytes:rgba
	       bytesPerRow:(NSUInteger)w * 4];
	if (mip && s_queue) {
		// GC decodes an authored mip chain; regenerating from level 0 is
		// visually equivalent for these box-filtered chains. Same queue as
		// the frame render, committed now -> ordered before any draw.
		id<MTLCommandBuffer> cb = [s_queue commandBuffer];
		id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
		[be generateMipmapsForTexture:tex];
		[be endEncoding];
		[cb commit];
	}
	if (getenv("PC_DUMP_TEX")) {
		static unsigned char* rb = (unsigned char*)malloc(1024 * 1024 * 4);
		if (rb && w <= 1024 && h <= 1024) {
			memset(rb, 0, (size_t)w * h * 4);
			[tex getBytes:rb bytesPerRow:(NSUInteger)w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
			double gs = 0; for (long i = 0; i < (long)w * h; i++) gs += rb[i*4] + rb[i*4+1] + rb[i*4+2];
			fprintf(stderr, "[TEXGPU] handle %d %dx%d gpu_mean=%.1f storage=%ld\n",
			        slot, w, h, gs / ((double)w * h * 3.0), (long)tex.storageMode);
		}
	}
	s_textures[slot] = tex;
	s_tex_samplers[slot] = pc_sampler(wrapS, wrapT, mip);
	{ double s = 0; for (long i = 0; i < (long)w * h; i++) s += rgba[i*4] + rgba[i*4+1] + rgba[i*4+2];
	  s_tex_mean[slot] = (float)(s / ((double)w * h * 3.0)); }
	return slot;
}

static const char* kShader = R"(
#include <metal_stdlib>
using namespace metal;
// Vertex carries only honest GX inputs: clip pos, the lit rasterized channel
// color (RAS), and the two generated texcoords. The TEV program rides in a
// per-segment uniform (TevU) and is evaluated exactly, stage by stage.
struct VOut { float4 pos [[position]]; float4 color; float4 spec; float2 st; float2 st2; };
vertex VOut vmain(const device float* v [[buffer(0)]], constant float& xscale [[buffer(1)]], uint vid [[vertex_id]]) {
	const device float* p = v + vid * 16;
	VOut o;
	// GX clip z is [-w, 0]; Metal wants [0, w]: z' = z + w.
	// xscale: widescreen narrows 2D (HUD) batches back to their 4:3 shape; 1 otherwise.
	o.pos = float4(p[0] * xscale, p[1], p[2] + p[3], p[3]);
	o.color = float4(p[4], p[5], p[6], p[7]);   // channel 0: lit diffuse
	o.spec = float4(p[12], p[13], p[14], p[15]); // channel 1: lit specular
	o.st = float2(p[8], p[9]);
	o.st2 = float2(p[10], p[11]);
	return o;
}
// PC_EFB_LOWRES: the scene renders into a 640x480 target (GC's EFB size) and
// this pair upscales it to the window, approximating GXSetCopyFilter's 7-tap
// vertical filter. Sub-pixel geometry (spinning particle ribbons) then melts
// into soft glow exactly as on hardware instead of rasterizing as hairlines.
struct UpsOut { float4 pos [[position]]; float2 uv; };
vertex UpsOut ups_v(uint vid [[vertex_id]]) {
	float2 xy = float2((vid << 1) & 2, vid & 2); // fullscreen triangle
	UpsOut o;
	o.pos = float4(xy * 2.0 - 1.0, 0.0, 1.0);
	// oversized triangle: uv must reach 1.0 at the SCREEN edge (xy=1), not
	// at the triangle's far vertices (xy=2)
	o.uv = float2(xy.x, 1.0 - xy.y);
	return o;
}
fragment float4 ups_f(UpsOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
	// GXSetCopyFilter vfilter {8,8,10,12,10,8,8}/64 spans ~3 EFB lines
	// (Pikmin's deflicker table): approximate with 3 taps at +/-1 line.
	float dy = 1.0 / 480.0;
	float4 c = t.sample(s, in.uv) * 0.5;
	c += t.sample(s, in.uv + float2(0.0, dy)) * 0.25;
	c += t.sample(s, in.uv - float2(0.0, dy)) * 0.25;
	return c;
}
// GXCopyTex resolve: sample a subrect of the scene target (rect = uv origin
// + size, EFB-normalized) into an EFB-copy texture.
vertex UpsOut copy_v(uint vid [[vertex_id]], constant float4& rect [[buffer(0)]]) {
	float2 xy = float2((vid << 1) & 2, vid & 2);
	UpsOut o;
	o.pos = float4(xy * 2.0 - 1.0, 0.0, 1.0);
	float2 base = float2(xy.x, 1.0 - xy.y);
	o.uv = rect.xy + base * rect.zw;
	return o;
}
fragment float4 copy_f(UpsOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
	return t.sample(s, in.uv);
}
// GX_TF_Z8 EFB copy: grab the DEPTH plane as intensity (GC Z-texture
// semantics: I in all channels incl. alpha). Pikmin's blur weights by this.
fragment float4 copyz_f(UpsOut in [[stage_in]], depth2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
	float d = t.sample(s, in.uv);
	return float4(d, d, d, d);
}
// Mirrors PcTevProg (pc_tev_prog.h) — the C side hands it over verbatim.
struct TevU {
	int4 hdr; // nstages, lastco, lastao, atest(comp|ref<<3)
	float4 regs[4];  // [0]=PREV, [1..3]=C0..C2
	float4 konst[4];
	int4 cargs[8]; // color args a,b,c,d
	int4 aargs[8]; // alpha args a,b,c,d
	int4 cops[8];  // op, bias, scale, out_reg
	int4 aops[8];
	int4 tex[8];   // texflag(0 none,1 sample,2 white), uvsel, chansel(0=zero,
	               // 1=diffuse COLOR0A0, 2=specular COLOR1A1), kcsel
	int4 misc[8];  // cclamp, aclamp, kasel, texhandle(unused here)
	int4 swap[8];    // ras swap table id, tex swap table id
	int4 swaptbl[4]; // 4 tables: r,g,b,a -> source channel (0=R 1=G 2=B 3=A)
};
// GXSetTevSwapModeTable swizzle: reorder/broadcast channels before the
// combiner (Pikmin uses {R,R,R,A} for desaturate/fade and damage flash).
static inline float4 pc_swizzle(float4 v, int4 t) {
	float src[4] = { v.r, v.g, v.b, v.a };
	return float4(src[t.x & 3], src[t.y & 3], src[t.z & 3], src[t.w & 3]);
}
// PC_ATEST: the pipeline variant enables discard; the actual GX comparator
// (comp0 + ref0, from GXSetAlphaCompare) rides in the program uniform so
// cutouts (GEQUAL 0x80) and particles (GEQUAL 1 — keep the soft fringe!) both
// behave exactly. GXCompare: 0 NEVER 1 LESS 2 EQUAL 3 LEQUAL 4 GREATER
// 5 NEQUAL 6 GEQUAL 7 ALWAYS.
constant bool PC_ATEST [[function_constant(0)]];
static inline float4 pc_atest(float4 c, int atw) {
	if (!PC_ATEST) return c;
	int comp = atw & 7;
	float ref = float(atw >> 3) / 255.0;
	float a = c.a;
	bool pass;
	switch (comp) {
	case 0: pass = false; break;
	case 1: pass = a < ref; break;
	case 2: pass = abs(a - ref) < 0.5 / 255.0; break;
	case 3: pass = a <= ref; break;
	case 4: pass = a > ref; break;
	case 5: pass = abs(a - ref) >= 0.5 / 255.0; break;
	case 6: pass = a >= ref; break;
	default: pass = true; break;
	}
	if (!pass) discard_fragment();
	return c;
}
// GXTevKColorSel: 0-7 = constants 1, 7/8 .. 1/8; 12-15 = K0-K3 rgb; 16+ = one
// channel of K0-K3 broadcast. (GXTevKAlphaSel identical minus the rgb rows.)
static inline float3 pc_kcsel(int sel, constant float4* konst) {
	if (sel < 8) return float3((8 - sel) / 8.0);
	if (sel >= 28) return float3(konst[sel - 28].a);
	if (sel >= 24) return float3(konst[sel - 24].b);
	if (sel >= 20) return float3(konst[sel - 20].g);
	if (sel >= 16) return float3(konst[sel - 16].r);
	if (sel >= 12) return konst[sel - 12].rgb;
	return float3(1.0);
}
static inline float pc_kasel(int sel, constant float4* konst) {
	if (sel < 8) return (8 - sel) / 8.0;
	if (sel >= 28) return konst[sel - 28].a;
	if (sel >= 24) return konst[sel - 24].b;
	if (sel >= 20) return konst[sel - 20].g;
	if (sel >= 16) return konst[sel - 16].r;
	return 1.0;
}
// GXTevColorArg -> value. NOTE the HARDWARE encoding interleaves color/alpha
// (2=C0, 3=A0, 4=C1, 5=A1, 6=C2, 7=A2 — Dolphin BPMemory / era SDK); this
// repo's GXEnum.h names are shifted (C0,C1,C2,A0..) but the VALUES flowing
// through GXSetTevColorIn are binary-authentic, so decode per hardware.
static inline float3 pc_cin(int arg, thread float4* regs, float4 texel, float4 ras, float3 kc) {
	switch (arg) {
	case 0: return regs[0].rgb;  // CPREV
	case 1: return regs[0].aaa;  // APREV
	case 2: return regs[1].rgb;  // C0
	case 3: return regs[1].aaa;  // A0
	case 4: return regs[2].rgb;  // C1
	case 5: return regs[2].aaa;  // A1
	case 6: return regs[3].rgb;  // C2
	case 7: return regs[3].aaa;  // A2
	case 8: return texel.rgb;    // TEXC
	case 9: return texel.aaa;    // TEXA
	case 10: return ras.rgb;     // RASC
	case 11: return ras.aaa;     // RASA
	case 12: return float3(1.0); // ONE
	case 13: return float3(0.5); // HALF
	case 14: return kc;          // KONST
	default: return float3(0.0); // ZERO
	}
}
static inline float pc_ain(int arg, thread float4* regs, float4 texel, float4 ras, float ka) {
	switch (arg) {
	case 0: return regs[0].a; // APREV
	case 1: return regs[1].a; // A0
	case 2: return regs[2].a; // A1
	case 3: return regs[3].a; // A2
	case 4: return texel.a;   // TEXA
	case 5: return ras.a;     // RASA
	case 6: return ka;        // KONST
	default: return 0.0;      // ZERO
	}
}
static inline float pc_scale(int sc) { return (sc == 1) ? 2.0 : ((sc == 2) ? 4.0 : ((sc == 3) ? 0.5 : 1.0)); }
static inline float pc_bias(int b) { return (b == 1) ? 0.5 : ((b == 2) ? -0.5 : 0.0); }
// The real GX TEV: evaluate every stage against a live register file.
//   ADD/SUB (op 0/1):  out = d [+/-] ((1-c)*a + c*b) + bias, * scale
//   compare (op 8-15): out = d + ((a OP b) ? c : 0)   (R8/GR16/BGR24/RGB8)
// then clamp and write the stage's output register. The pixel is the color/
// alpha of the registers written by the final stage.
fragment float4 fmain_tev(VOut in [[stage_in]],
                          constant TevU& U [[buffer(0)]],
                          array<texture2d<float>, 8> texs [[texture(0)]],
                          array<sampler, 8> smps [[sampler(0)]]) {
	float4 regs[4];
	regs[0] = U.regs[0]; regs[1] = U.regs[1]; regs[2] = U.regs[2]; regs[3] = U.regs[3];
	int n = U.hdr.x;
	for (int i = 0; i < n; i++) {
		float4 texel = float4(0.0); // GX_TEXMAP_NULL: texel = 0
		int tf = U.tex[i].x;
		if (tf == 1) texel = texs[i].sample(smps[i], (U.tex[i].y == 1) ? in.st2 : in.st);
		else if (tf == 2) texel = float4(1.0); // missing on our side: neutral
		float4 ras = (U.tex[i].z == 2) ? in.spec : ((U.tex[i].z == 1) ? in.color : float4(0.0));
		// GX applies the stage's swap tables to BOTH inputs before combining.
		texel = pc_swizzle(texel, U.swaptbl[U.swap[i].y & 3]);
		ras = pc_swizzle(ras, U.swaptbl[U.swap[i].x & 3]);
		float3 kc = pc_kcsel(U.tex[i].w, U.konst);
		float ka = pc_kasel(U.misc[i].z, U.konst);
		float3 a = pc_cin(U.cargs[i].x, regs, texel, ras, kc);
		float3 b = pc_cin(U.cargs[i].y, regs, texel, ras, kc);
		float3 c = pc_cin(U.cargs[i].z, regs, texel, ras, kc);
		float3 d = pc_cin(U.cargs[i].w, regs, texel, ras, kc);
		int cop = U.cops[i].x;
		float3 cres;
		if (cop >= 8) {
			bool eq = (cop & 1) != 0;
			int dim = (cop - 8) >> 1;
			if (dim == 3) { // RGB8: per-component
				float3 t = select(float3(0.0), float3(1.0),
				                  eq ? (abs(a - b) < float3(0.5 / 255.0)) : (a > b + float3(0.25 / 255.0)));
				cres = d + c * t;
			} else {
				float x, y;
				if (dim == 0) { x = a.r; y = b.r; }
				else if (dim == 1) { x = dot(a.rg, float2(1.0, 256.0)); y = dot(b.rg, float2(1.0, 256.0)); }
				else { x = dot(a.rgb, float3(1.0, 256.0, 65536.0)); y = dot(b.rgb, float3(1.0, 256.0, 65536.0)); }
				bool t = eq ? (abs(x - y) < 0.5 / 255.0) : (x > y + 0.25 / 255.0);
				cres = d + (t ? c : float3(0.0));
			}
		} else {
			float3 lin = mix(a, b, c); // (1-c)*a + c*b
			cres = ((cop == 1) ? (d - lin) : (d + lin)) + pc_bias(U.cops[i].y);
			cres *= pc_scale(U.cops[i].z);
		}
		cres = (U.misc[i].x == 1) ? clamp(cres, 0.0, 1.0) : clamp(cres, -4.0, 4.0);
		float xa = pc_ain(U.aargs[i].x, regs, texel, ras, ka);
		float xb = pc_ain(U.aargs[i].y, regs, texel, ras, ka);
		float xc = pc_ain(U.aargs[i].z, regs, texel, ras, ka);
		float xd = pc_ain(U.aargs[i].w, regs, texel, ras, ka);
		int aop = U.aops[i].x;
		float ares;
		if (aop >= 8) {
			bool eq = (aop & 1) != 0;
			bool t = eq ? (abs(xa - xb) < 0.5 / 255.0) : (xa > xb + 0.25 / 255.0);
			ares = xd + (t ? xc : 0.0);
		} else {
			float lin = mix(xa, xb, xc);
			ares = ((aop == 1) ? (xd - lin) : (xd + lin)) + pc_bias(U.aops[i].y);
			ares *= pc_scale(U.aops[i].z);
		}
		ares = (U.misc[i].y == 1) ? clamp(ares, 0.0, 1.0) : clamp(ares, -4.0, 4.0);
		regs[U.cops[i].w].rgb = cres; // both equations read, THEN both write
		regs[U.aops[i].w].a = ares;
	}
	float4 outc = float4(regs[U.hdr.y].rgb, regs[U.hdr.z].a);
	return pc_atest(saturate(outc), U.hdr.w);
}
)";

// ---- iOS app lifecycle --------------------------------------------------
// Submitting GPU work while backgrounded is grounds for iOS to kill the app,
// and CoreAudio should not be rendering into a deactivated audio session
// either. So the frame loop parks while we are in the background.
//
// This is handled in an SDL EVENT WATCH rather than in the poll loop below,
// because SDL_APP_WILLENTERBACKGROUND is dispatched from the UIKit delegate
// callback and the process can be suspended before the main loop comes round
// again — by which point reacting is too late. The watch runs synchronously at
// the moment the callback fires, which is the only reliable point.
//
// Deliberately NOT handled: surviving a long stay in the background. If iOS
// reclaims the memory or kills the app, the day's progress is gone — Pikmin
// only writes the card at day end, so there is no valid mid-day save to make,
// and faking one is worse than losing it.
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h> // background task assertion for the save-sync release

extern "C" void pc_audio_set_host_suspended(int); // pc_audio.cpp

static SDL_atomic_t s_backgrounded;

// Releasing the save lease on the way out is not optional on iOS: atexit
// never runs here (a suspended app is killed, not exited), so without this
// the lease sits until the server's TTL sweeps it — and every one of those
// sweeps marks the save dirty and locks the other two devices out in the
// meantime. This IS the shutdown path on iOS.
//
// The upload needs more than the ~5s the system grants before suspension, so
// it runs inside a background task assertion (~30s). The handler must end the
// task itself or the app is killed for overrunning; sync_suspend's own
// timeouts are set well inside that budget.
static void pc_sync_release_in_background(void) {
	if (!pc_sync_enabled()) return;

	UIApplication* app = [UIApplication sharedApplication];
	__block UIBackgroundTaskIdentifier task = UIBackgroundTaskInvalid;
	task = [app beginBackgroundTaskWithName:@"pikmin-save-sync"
	                      expirationHandler:^{
		                      // Out of time. End the assertion so the app is
		                      // suspended cleanly rather than killed; the
		                      // lease then expires by TTL, which the next
		                      // reconcile handles.
		                      fprintf(stderr, "[PC] *** save sync ran out of background time\n");
		                      if (task != UIBackgroundTaskInvalid) {
			                      [app endBackgroundTask:task];
			                      task = UIBackgroundTaskInvalid;
		                      }
	                      }];

	pc_sync_suspend();

	if (task != UIBackgroundTaskInvalid) {
		[app endBackgroundTask:task];
		task = UIBackgroundTaskInvalid;
	}
}

static int SDLCALL pc_lifecycle_watch(void*, SDL_Event* ev) {
	switch (ev->type) {
	case SDL_APP_WILLENTERBACKGROUND:
		SDL_AtomicSet(&s_backgrounded, 1);
		pc_audio_set_host_suspended(1);
		fprintf(stderr, "[PC] backgrounded — parking the frame loop\n");
		// After the frame loop is parked, so the card is quiescent and the
		// blob we upload cannot be a half-written save.
		pc_sync_release_in_background();
		break;
	case SDL_APP_DIDENTERFOREGROUND:
		SDL_AtomicSet(&s_backgrounded, 0);
		pc_audio_set_host_suspended(0);
		fprintf(stderr, "[PC] foregrounded — resuming\n");
		// Retake the lease and reconcile: another device may have played,
		// saved and uploaded while this one was away.
		pc_sync_resume();
		break;
	default:
		break;
	}
	return 1; // keep the event queued for the normal poll loop too
}

// Park until foreground. SDL_Delay does NOT pump events, and on iOS the
// foreground callback only arrives while the run loop runs (UIKit_PumpEvents
// spins CFRunLoop), so the pump call is what lets this loop ever exit.
static void pc_wait_while_backgrounded(void) {
	if (!SDL_AtomicGet(&s_backgrounded)) return;
	while (SDL_AtomicGet(&s_backgrounded)) {
		SDL_PumpEvents();
		SDL_Delay(100);
	}
}
#else
static void pc_wait_while_backgrounded(void) { }
#endif

static bool pc_window_init(void) {
	if (s_window) return true;
	if (s_init_failed) return false;

	if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
		fprintf(stderr, "[PC] SDL video init failed: %s\n", SDL_GetError());
		s_init_failed = true;
		return false;
	}
	// Gamepads are optional: a failure here must not stop the game booting.
	if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
		fprintf(stderr, "[PC] SDL gamecontroller init failed (keyboard only): %s\n", SDL_GetError());
	} else {
		pc_pad_open_first();
	}
	// video.scale is a multiple of the GameCube's 640x480; video.fullscreen
	// uses desktop-resolution fullscreen (no mode switch).
	int scale = pc_config_int("video.scale", 2);
	if (scale < 1) scale = 1;
	if (scale > 6) scale = 6;
	// RESIZABLE is what enables macOS's green fullscreen button — AppKit greys
	// it out otherwise, which left settings.ini + a relaunch as the only way
	// into fullscreen. It is only safe to allow arbitrary window sizes because
	// the scene is now letterboxed into a 4:3 viewport; without that, resizing
	// would stretch the image.
	Uint32 wflags = SDL_WINDOW_METAL | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE;
	if (pc_config_int("video.fullscreen", 0)) wflags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
	s_window = SDL_CreateWindow("Pikmin", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640 * scale, 480 * scale,
	                            wflags);
	if (!s_window) {
		fprintf(stderr, "[PC] SDL window failed: %s\n", SDL_GetError());
		s_init_failed = true;
		return false;
	}
#if TARGET_OS_IPHONE
	// Registered before the layer exists: the watch only sets flags, and having
	// it live for the whole run means a background event during init cannot be
	// missed. See pc_lifecycle_watch for why this is a watch and not a poll.
	SDL_AddEventWatch(pc_lifecycle_watch, NULL);
#endif
	SDL_MetalView view = SDL_Metal_CreateView(s_window);
	s_layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(view);
	// Enable Metal programmatic GPU capture unconditionally (must be set before
	// the device exists) so the F10/G trace hotkey works without launching with
	// MTL_CAPTURE_ENABLED=1 by hand.
	setenv("MTL_CAPTURE_ENABLED", "1", 0);
	s_device = MTLCreateSystemDefaultDevice();
	s_layer.device = s_device;
	s_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	s_layer.framebufferOnly = NO; // allow blit readback (PC_DUMP_FRAMES + F9 capture)
	s_queue = [s_device newCommandQueue];
	NSError* err = nil;
	id<MTLLibrary> lib = [s_device newLibraryWithSource:[NSString stringWithUTF8String:kShader]
	                                            options:nil
	                                              error:&err];
	if (!lib) {
		fprintf(stderr, "[PC] shader compile failed: %s\n", [[err localizedDescription] UTF8String]);
		s_init_failed = true;
		return false;
	}
	s_lib = lib;
	s_vfn = [lib newFunctionWithName:@"vmain"];
	// PSOs are built lazily per (fragfunc, blendcode, alpha-test) in pc_get_pso.
	for (int i = 0; i < 4; i++) {
		MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
		dd.depthCompareFunction = (i & 1) ? MTLCompareFunctionLessEqual : MTLCompareFunctionAlways;
		dd.depthWriteEnabled = (i & 2) ? YES : NO;
		s_ds[i] = [s_device newDepthStencilStateWithDescriptor:dd];
	}
	{
		int ww = 0, wh = 0;
		SDL_GetWindowSize(s_window, &ww, &wh);
		const CGSize ds = s_layer.drawableSize;
		fprintf(stderr, "[PC] window up: %s — window %dx%d, drawable %.0fx%.0f%s\n", [[s_device name] UTF8String], ww,
		        wh, ds.width, ds.height, (wflags & SDL_WINDOW_FULLSCREEN_DESKTOP) ? ", fullscreen" : "");
	}
	return true;
}

// Keyboard -> GC pad. Arrows/WASD = stick, IJKL = C-stick, Enter = Start,
// Space/X = A, LShift/C = B, Z = Z, Q/E = L/R triggers.
extern "C" void pc_window_get_pad(unsigned short* buttons, signed char* stickX, signed char* stickY, signed char* subX,
                                  signed char* subY, unsigned char* trigL, unsigned char* trigR) {
	*buttons = 0;
	*stickX = *stickY = *subX = *subY = 0;
	*trigL = *trigR = 0;
	if (!s_window) return;
	// Refresh SDL's level state now. Without this the sample is whatever the
	// pump at the END of the previous frame left behind — a whole frame of
	// avoidable input latency, since PADRead runs before this frame's pump.
	// Only the main thread may pump (same rule as presenting); today PADRead
	// is main-thread-only, so this is a guard against future callers rather
	// than a known case.
	if (pc_is_main_thread()) SDL_PumpEvents();
	pc_bindings_init();
	const Uint8* k = SDL_GetKeyboardState(NULL);
	unsigned short b = 0;
	for (int i = 0; i < s_nkey_map; i++)
		if (k[s_key_map[i].scancode]) b |= s_key_map[i].bit;
	if (b & 0x0040) *trigL = 255; // keyboard L/R are all-or-nothing triggers
	if (b & 0x0020) *trigR = 255;
	// Index order matches pc_bindings_init: 0=up 1=down 2=left 3=right.
	int sx = 0, sy = 0;
	if (pc_dir_down(k, s_stick_keys[2], s_stick_n[2])) sx -= 1;
	if (pc_dir_down(k, s_stick_keys[3], s_stick_n[3])) sx += 1;
	if (pc_dir_down(k, s_stick_keys[0], s_stick_n[0])) sy += 1;
	if (pc_dir_down(k, s_stick_keys[1], s_stick_n[1])) sy -= 1;
	*stickX = (signed char)(sx * 90);
	*stickY = (signed char)(sy * 90);
	int cx = 0, cy = 0;
	if (pc_dir_down(k, s_cstick_keys[2], s_cstick_n[2])) cx -= 1;
	if (pc_dir_down(k, s_cstick_keys[3], s_cstick_n[3])) cx += 1;
	if (pc_dir_down(k, s_cstick_keys[0], s_cstick_n[0])) cy += 1;
	if (pc_dir_down(k, s_cstick_keys[1], s_cstick_n[1])) cy -= 1;
	*subX = (signed char)(cx * 90);
	*subY = (signed char)(cy * 90);

	// ---- gamepad, OR-ed on top of the keyboard --------------------------
	// Layout follows the usual GameCube-on-a-modern-pad convention:
	//   cross->A  circle->B  square->X  triangle->Y  Options->Start
	//   L1->Z (GC's Z is a digital shoulder), L2/R2 -> the GC's ANALOG L/R
	//   left stick -> main stick, right stick -> C-stick, dpad -> dpad.
	// Pikmin needs the analog triggers (camera / sideways-view) and both
	// sticks, which is exactly why keyboard play is rough.
	if (s_pad) {
		for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++)
			if (SDL_GameControllerGetButton(s_pad, (SDL_GameControllerButton)i)) b |= pc_pad_bit(i);

		// Triggers: GC reports 0..255 analog plus a digital bit that latches
		// near full pull (PAD_TRIGGER_L/R), which is what the game tests for
		// the discrete actions. Shoulder buttons also drive the digital bit
		// so R1/L1-style play works.
		int lt = SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
		int rt = SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
		if (lt > 0) {
			int v = lt * 255 / 32767;
			if (v > *trigL) *trigL = (unsigned char)(v > 255 ? 255 : v);
		}
		if (rt > 0) {
			int v = rt * 255 / 32767;
			if (v > *trigR) *trigR = (unsigned char)(v > 255 ? 255 : v);
		}
		if (lt > 26000) b |= 0x0040;                                                           // L click
		if (rt > 26000) b |= 0x0020;                                                           // R click
		if (SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) {
			b |= 0x0020;
			if (*trigR < 255) *trigR = 255;
		}

		// Sticks: SDL is +/-32767 with Y DOWN-positive; the GC pad is
		// +/-~90 usable with Y UP-positive. Radial deadzone (not per-axis,
		// which would square off diagonals and make Olimar walk on 8 axes).
		const float DEAD = 6000.0f, MAXV = 32767.0f, SCALE = 90.0f;
		struct {
			SDL_GameControllerAxis ax, ay;
			signed char *ox, *oy;
		} sticks[2] = { { SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, stickX, stickY },
			            { SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY, subX, subY } };
		for (int i = 0; i < 2; i++) {
			float ax = (float)SDL_GameControllerGetAxis(s_pad, sticks[i].ax);
			float ay = (float)SDL_GameControllerGetAxis(s_pad, sticks[i].ay);
			float mag = sqrtf(ax * ax + ay * ay);
			if (mag <= DEAD) continue; // inside deadzone: leave the keyboard's value
			// rescale so the stick still reaches full range past the deadzone
			float scaled = (mag - DEAD) / (MAXV - DEAD);
			if (scaled > 1.0f) scaled = 1.0f;
			float nx = (ax / mag) * scaled * SCALE;
			float ny = -(ay / mag) * scaled * SCALE; // invert: SDL Y down, GC Y up
			*sticks[i].ox = (signed char)(nx < -90 ? -90 : (nx > 90 ? 90 : nx));
			*sticks[i].oy = (signed char)(ny < -90 ? -90 : (ny > 90 ? 90 : ny));
		}
	}

	// ---- touch overlay, OR-ed on last ------------------------------------
	// A connected controller hides the overlay entirely, so the two cannot
	// fight; the pad is the source of truth whenever one is present.
	pc_touch_pad_set_controller(s_pad != NULL);
	pc_touch_pad_apply(&b, stickX, stickY, subX, subY, trigL, trigR);

	// Presses caught by the event loop since the last read: OR them in so a
	// tap shorter than one frame still registers, then clear so each latched
	// press is visible for exactly one PADRead.
	b |= s_input_latch;
	s_input_latch = 0;

	*buttons = b;
}

// On-demand capture (debug key F9): dump the current frame's PNG + segment list.
// Set from the event loop; read by pc_gx_frame_flush (segs) and the PNG dump
// below, then cleared once the PNG is written so both cover the same frame.
static int s_capture_req = 0;
static int s_gpu_capture_req = 0; // F10: Metal GPU trace of the next frame
extern "C" int pc_capture_pending(void) { return s_capture_req; }


extern "C" void pc_window_frame(void) {
	if (!pc_window_init()) return;

	// Before anything touches the drawable or the event queue.
	pc_wait_while_backgrounded();

	// The overlay's layout is anchored to the screen edges, and finger events
	// arrive in window-normalised coords — both need the current aspect, so
	// refresh it before the poll loop below feeds those events in.
	{
		const CGSize ds = s_layer.drawableSize;
		if (ds.height > 0) pc_touch_pad_set_aspect((float)(ds.width / ds.height));
		// Wall-clock, not a sim tick: the overlay dims on how long the player has
		// not touched it, which has to keep running while the game is paused.
		static Uint32 s_overlay_ms;
		const Uint32 now = SDL_GetTicks();
		if (s_overlay_ms) pc_touch_pad_advance((float)(now - s_overlay_ms) * 0.001f);
		s_overlay_ms = now;
	}

	// Snapshot the capture flag BEFORE polling events: an F9 pressed this frame
	// sets it for NEXT frame, so pc_gx_frame_flush (which runs before this call
	// on the following retrace) and the PNG dump below land on the same frame.
	bool capture = s_capture_req;

	SDL_Event ev;
	while (SDL_PollEvent(&ev)) {
		if (ev.type == SDL_QUIT) {
			fprintf(stderr, "[PC] quit requested\n");
			exit(0);
		}
		// Fullscreen toggle: Cmd-F (macOS convention), F11 and Alt-Enter (the
		// game convention). The green button works too now that the window is
		// resizable, but a key is what people actually reach for mid-game.
		if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
			const SDL_Keycode k = ev.key.keysym.sym;
			const Uint16 mod    = ev.key.keysym.mod;
			const bool toggle   = (k == SDLK_F11) || (k == SDLK_f && (mod & KMOD_GUI))
			                    || (k == SDLK_RETURN && (mod & KMOD_ALT));
			if (toggle) {
				const bool fs = (SDL_GetWindowFlags(s_window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
				SDL_SetWindowFullscreen(s_window, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
				fprintf(stderr, "[PC] %s fullscreen\n", fs ? "left" : "entered");
				continue; // never let Cmd-F reach the game as a bound key
			}
		}

		// ESCAPE NO LONGER QUITS. It used to call exit(0) outright — no confirm,
		// no save — and it sits next to nothing you would ever want mid-game, so
		// brushing it threw away the session. Quitting is Cmd-Q or the close
		// button, both of which are deliberate.
		//
		// In fullscreen Escape now does what people reach for it to do: drop back
		// to a window. That keeps an escape hatch (fullscreen with no way out
		// except Cmd-Q is its own trap) without it being destructive.
		if (ev.type == SDL_KEYDOWN && !ev.key.repeat && ev.key.keysym.sym == SDLK_ESCAPE) {
			if (SDL_GetWindowFlags(s_window) & SDL_WINDOW_FULLSCREEN_DESKTOP) {
				SDL_SetWindowFullscreen(s_window, 0);
				fprintf(stderr, "[PC] left fullscreen (Cmd-Q quits)\n");
			}
			// falls through so a user-bound Escape still reaches the latch below
		}
		// Latch presses as they arrive (see s_input_latch): the level read in
		// pc_window_get_pad only sees buttons still held at sample time.
		// Key repeats are ignored — a held key is already covered by level.
		if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
			s_input_latch |= pc_key_bit(ev.key.keysym.scancode);
		}
		if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
			s_input_latch |= pc_pad_bit(ev.cbutton.button);
		}
		if (ev.type == SDL_FINGERDOWN || ev.type == SDL_FINGERMOTION || ev.type == SDL_FINGERUP) {
			pc_touch_pad_handle_event(&ev);
		}
		if (ev.type == SDL_CONTROLLERDEVICEADDED) {
			pc_pad_open_first();
		}
		if (ev.type == SDL_CONTROLLERDEVICEREMOVED) {
			pc_pad_closed(ev.cdevice.which);
		}
		if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F9) {
			s_capture_req = 1; // dumps next frame (flush runs before this poll)
			fprintf(stderr, "[PC] F9: capturing next frame -> pc_capture.png + segs on stderr\n");
		}
		if (ev.type == SDL_KEYDOWN && (ev.key.keysym.sym == SDLK_F10 || ev.key.keysym.sym == SDLK_g)) {
			// F10: on-demand Metal GPU trace of the next frame (plus the F9-style
			// PNG + seg dump so the trace pairs with per-draw state). Requires
			// launching with MTL_CAPTURE_ENABLED=1.
			s_gpu_capture_req = 1;
			s_capture_req = 1;
			fprintf(stderr, "[PC] F10: GPU-tracing next frame -> pikmin.gputrace (+ pc_capture.png/segs)\n");
		}
	}

	@autoreleasepool {
		id<CAMetalDrawable> drawable = [s_layer nextDrawable];
		if (!drawable) {
			// No drawable: this frame is abandoned. It used to return with
			// s_tri_verts / s_draw_nsegs still holding the frame's geometry, so
			// the NEXT frame appended to it and drew both — the game resubmits
			// everything every frame, so the carried-over copy is pure
			// duplication, showing last frame's objects at last frame's positions
			// underneath the current ones for exactly one frame.
			//
			// Flicker-shaped, and it fires on GPU/compositor timing rather than
			// on anything in the recording, which is the profile of a fault that
			// shows in live play and not in replay. It also inflated a frame's
			// vertex demand to roughly two frames' worth, so it is a way to cross
			// a cap that no single frame comes near.
			//
			// Drop the frame instead. Resetting the demand counters with it keeps
			// them per-frame like the shim's, and stops the pair reading "ok"
			// while the buffer actually holds two frames.
			if (s_tri_verts || s_draw_nsegs) {
				s_drawable_miss++;
				if (s_drawable_miss <= 3 || (s_drawable_miss % 100) == 0)
					fprintf(stderr, "[PC] no drawable — frame dropped at tick %u (%d verts / %d segs discarded, "
					                "miss #%d)\n",
					        pc_input_current_tick(), s_tri_verts, s_draw_nsegs, s_drawable_miss);
			}
			s_tri_verts = 0;
			s_draw_nsegs = 0;
			s_want_verts = 0;
			s_want_segs = 0;
			return;
		}
		// PC_EFB_LOWRES=1: render the scene at GC's 640x480 EFB size and
		// upscale through a copy-filter approximation (see ups_f). This is
		// the missing PRESENTATION class: GC undersamples sub-pixel geometry
		// (spinning ribbons vanish/soften) then vertical-filters the copy-out;
		// raw high-res presents the same scene as crisp hairlines.
		static int s_lowres = -1;
		if (s_lowres < 0) s_lowres = getenv("PC_EFB_LOWRES") ? 1 : 0;
		id<MTLTexture> sceneTarget = drawable.texture;
		if (s_lowres) {
			if (!s_efb_tex) {
				MTLTextureDescriptor* et = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
				                                                                              width:640
				                                                                             height:480
				                                                                          mipmapped:NO];
				et.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
				et.storageMode = MTLStorageModePrivate;
				s_efb_tex = [s_device newTextureWithDescriptor:et];
			}
			sceneTarget = s_efb_tex;
		}
		if (!s_depth_tex || s_depth_tex.width != sceneTarget.width || s_depth_tex.height != sceneTarget.height) {
			MTLTextureDescriptor* dt = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
			                                                                              width:sceneTarget.width
			                                                                             height:sceneTarget.height
			                                                                          mipmapped:NO];
			// ShaderRead: GX_TF_Z8 EFB copies sample the depth plane
			dt.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
			dt.storageMode = MTLStorageModePrivate;
			s_depth_tex = [s_device newTextureWithDescriptor:dt];
		}
		// LETTERBOX. GXSetViewport is a no-op here, so the game's NDC output maps
		// to the WHOLE render target. That is fine while the window is 4:3 (which
		// video.scale guarantees — it is a multiple of 640x480), but fullscreen
		// hands us the display's aspect: measured 3024x1834 on this machine,
		// which is 1.649:1 against 4:3 and stretches the image ~24% horizontally.
		//
		// Constrain rasterisation to a centred 4:3 rect. The attachment is still
		// cleared in full, so the bars carry the game's own clear colour rather
		// than black — geometrically correct, and one line, where true black bars
		// would need a second clear plus a solid-colour quad inside the viewport.
		MTLViewport s_letterbox;
		bool s_do_letterbox = false;
		{
			const double tw = (double)sceneTarget.width, th = (double)sceneTarget.height;
			const double want = 4.0 / 3.0;
			s_ws_scale = (pc_widescreen_enabled() && th > 0.0 && tw / th > want + 0.01) ? (float)((tw / th) / want) : 1.0f;
			if (tw > 0.0 && th > 0.0 && fabs(tw / th - want) > 0.001) {
				double vw = tw, vh = th;
				if (tw / th > want) {
					vw = th * want; // too wide — pillarbox
				} else {
					vh = tw / want; // too tall — letterbox
				}
				s_letterbox = (MTLViewport) { (tw - vw) * 0.5, (th - vh) * 0.5, vw, vh, 0.0, 1.0 };
				s_do_letterbox = true;
			}
		}

		MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.depthAttachment.texture = s_depth_tex;
		pass.depthAttachment.loadAction = MTLLoadActionClear;
		// Store: the scene pass may split around GXCopyTex resolves and
		// resume with loadAction Load.
		pass.depthAttachment.storeAction = MTLStoreActionStore;
		pass.depthAttachment.clearDepth = 1.0;
		pass.colorAttachments[0].texture = sceneTarget;
		pass.colorAttachments[0].loadAction = MTLLoadActionClear;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		pass.colorAttachments[0].clearColor = MTLClearColorMake(s_clear[0], s_clear[1], s_clear[2], 1.0);
		// PC_GPU_CAPTURE=N: write frame N's GPU work to ./pikmin.gputrace (open
		// in Xcode: File > Open). Standalone — no Xcode project needed. Requires
		// MTL_CAPTURE_ENABLED=1 in the environment. Frame N matches the
		// PC_DUMP_FRAMES numbering (both count presents).
		static long s_capnum = 0;
		s_capnum++;
		bool docapture = false;
		bool wantcapture = s_gpu_capture_req != 0; // F10 hotkey
		s_gpu_capture_req = 0;
		if (const char* cf = getenv("PC_GPU_CAPTURE"))
			if (s_capnum == atol(cf)) wantcapture = true;
		{
			if (wantcapture) {
				MTLCaptureManager* cm = [MTLCaptureManager sharedCaptureManager];
				if ([cm respondsToSelector:@selector(supportsDestination:)]
				    && [cm supportsDestination:MTLCaptureDestinationGPUTraceDocument]) {
					MTLCaptureDescriptor* cd = [[MTLCaptureDescriptor alloc] init];
					cd.captureObject = s_queue;
					cd.destination = MTLCaptureDestinationGPUTraceDocument;
					[[NSFileManager defaultManager] removeItemAtPath:@"pikmin.gputrace" error:nil];
					cd.outputURL = [NSURL fileURLWithPath:@"pikmin.gputrace"];
					NSError* cerr = nil;
					if ([cm startCaptureWithDescriptor:cd error:&cerr]) {
						docapture = true;
						fprintf(stderr, "[GPU] capturing frame %ld -> pikmin.gputrace\n", s_capnum);
					} else {
						fprintf(stderr, "[GPU] startCapture failed: %s (need MTL_CAPTURE_ENABLED=1)\n",
						        cerr.localizedDescription.UTF8String);
					}
				} else {
					fprintf(stderr, "[GPU] gputrace unsupported here (need MTL_CAPTURE_ENABLED=1)\n");
				}
			}
		}
		id<MTLCommandBuffer> cmd = [s_queue commandBuffer];
		id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];
		pc_bind_tev_defaults(enc);
		// Viewport per segment (see widescreen notes): -1 = encoder default (full).
		const bool ws = s_ws_scale > 1.0f;
		int vp_mode = -1;
		auto seg_is_rtt = [&](int seg) -> bool {
			// The first copy event after this segment is the one that captures it.
			for (int e = 0; e < s_ncopy_events; e++)
				if (s_copy_events[e].after_seg > seg)
					return s_copy_events[e].sw < 620 || s_copy_events[e].sh < 460;
			return false;
		};
		auto set_vp = [&](int want) {
			if (want == vp_mode) return;
			if (want == 1) [enc setViewport:s_letterbox];
			else [enc setViewport:(MTLViewport){ 0.0, 0.0, (double)sceneTarget.width, (double)sceneTarget.height, 0.0, 1.0 }];
			vp_mode = want;
		};
		if (s_do_letterbox && !ws) set_vp(1);
		{ const float one = 1.0f; [enc setVertexBytes:&one length:sizeof(one) atIndex:1]; }
		if (getenv("PC_DIAG_DRAW")) {
			static int s_diag_frame;
			if ((s_diag_frame++ % 60) == 0)
				fprintf(stderr, "[DIAG] draw: nsegs=%d verts=%d rejects=%d clear=%.2f,%.2f,%.2f\n",
				        s_draw_nsegs, s_tri_verts, s_diag_rejects, s_clear[0], s_clear[1], s_clear[2]);
			s_diag_rejects = 0;
		}
		if (getenv("PC_DIAG_SEG")) {
			static int s_seg_shot = 0;
			// Fire once at the in-world day-1 frame: a big depth-write batch
			// textured from the high (terrain) handle range.
			bool world = false;
			for (int i = 0; i < s_draw_nsegs; i++)
				if ((s_draw_segs[i].depth & 3) == 3 && s_draw_segs[i].count > 3000 && s_draw_segs[i].tex >= 60) { world = true; break; }
			if (world && !s_seg_shot) { s_seg_shot = 1;
				for (int i = 0; i < s_draw_nsegs; i++) {
					int t = s_draw_segs[i].tex;
					fprintf(stderr, "[SEGTEX] seg %02d tex=%-4d mean=%.1f count=%d depth=%d\n", i, t,
					        (t >= 0 && t < s_ntextures) ? s_tex_mean[t] : -1.0f, s_draw_segs[i].count, s_draw_segs[i].depth);
				}
			}
		}
		// GXCopyTex resolve: end the scene encoder, render the scene-so-far
		// into the grab's texture, reopen the scene pass (Load, or Clear if
		// the copy requested an EFB clear).
		int next_ev = 0;
		auto resolve_copy = [&](int ev) {
			static int s_resolvelog;
			if (getenv("PC_VERBOSE") && s_resolvelog < 4) {
				s_resolvelog++;
				fprintf(stderr, "[EFBcopy] resolve slot=%d handle=%d after_seg=%d clear=%d\n",
				        s_copy_events[ev].slot, s_copies[s_copy_events[ev].slot].handle,
				        s_copy_events[ev].after_seg, s_copy_events[ev].clear);
			}
			[enc endEncoding];
			bool depthCopy = s_copy_events[ev].isDepth != 0;
			if (!s_copy_pso) {
				MTLRenderPipelineDescriptor* cd2 = [MTLRenderPipelineDescriptor new];
				cd2.vertexFunction = [s_lib newFunctionWithName:@"copy_v"];
				cd2.fragmentFunction = [s_lib newFunctionWithName:@"copy_f"];
				cd2.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
				NSError* cerr2 = nil;
				s_copy_pso = [s_device newRenderPipelineStateWithDescriptor:cd2 error:&cerr2];
				if (!s_copy_pso)
					fprintf(stderr, "[PC] copy pso failed: %s\n", [[cerr2 localizedDescription] UTF8String]);
				cd2.fragmentFunction = [s_lib newFunctionWithName:@"copyz_f"];
				s_copyz_pso = [s_device newRenderPipelineStateWithDescriptor:cd2 error:&cerr2];
				if (!s_copyz_pso)
					fprintf(stderr, "[PC] copyz pso failed: %s\n", [[cerr2 localizedDescription] UTF8String]);
				MTLSamplerDescriptor* zsd = [MTLSamplerDescriptor new]; // nearest/clamp
				s_depth_sampler = [s_device newSamplerStateWithDescriptor:zsd];
			}
			id<MTLRenderPipelineState> pso = depthCopy ? s_copyz_pso : s_copy_pso;
			if (pso) {
				MTLRenderPassDescriptor* cp = [MTLRenderPassDescriptor renderPassDescriptor];
				cp.colorAttachments[0].texture = s_textures[s_copies[s_copy_events[ev].slot].handle];
				cp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
				cp.colorAttachments[0].storeAction = MTLStoreActionStore;
				id<MTLRenderCommandEncoder> ce = [cmd renderCommandEncoderWithDescriptor:cp];
				// GXSetTexCopySrc coords are EFB (640x480) pixels, expressed here as
				// a fraction of the render target.
				//
				// LETTERBOXING MOVES THIS SEAM TOO. When the target is not 4:3 the
				// scene is rasterised into a centred sub-rect, so a copy addressed
				// as a fraction of the WHOLE target samples the wrong region — and
				// includes the bars. the prior port's notes call this out explicitly:
				// letterboxing touches three coordinate seams, not one (viewport,
				// scissor, EFB-copy source rect). Scissor is a no-op in this port,
				// but this one is real, and only shows in fullscreen.
				float rx = s_copy_events[ev].sx / 640.0f, ry = s_copy_events[ev].sy / 480.0f;
				float rw = s_copy_events[ev].sw / 640.0f, rh = s_copy_events[ev].sh / 480.0f;
				const bool copy_rtt = s_copy_events[ev].sw < 620 || s_copy_events[ev].sh < 460;
				if (s_do_letterbox && (!ws || copy_rtt)) {
					const float tw = (float)sceneTarget.width, th = (float)sceneTarget.height;
					const float vx = (float)s_letterbox.originX / tw, vy = (float)s_letterbox.originY / th;
					const float vw = (float)s_letterbox.width / tw, vh = (float)s_letterbox.height / th;
					rx = vx + rx * vw; ry = vy + ry * vh;
					rw *= vw;          rh *= vh;
				}
				float rect[4] = { rx, ry, rw, rh };
				[ce setRenderPipelineState:pso];
				[ce setVertexBytes:rect length:sizeof(rect) atIndex:0];
				[ce setFragmentTexture:(depthCopy ? s_depth_tex : sceneTarget) atIndex:0];
				[ce setFragmentSamplerState:(depthCopy ? s_depth_sampler : pc_sampler(0, 0, 0)) atIndex:0];
				[ce drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
				[ce endEncoding];
			}
			MTLLoadAction la = s_copy_events[ev].clear ? MTLLoadActionClear : MTLLoadActionLoad;
			pass.colorAttachments[0].loadAction = la;
			pass.depthAttachment.loadAction = la;
			enc = [cmd renderCommandEncoderWithDescriptor:pass];
			pc_bind_tev_defaults(enc); // a fresh encoder starts with nothing bound
			vp_mode = -1; // fresh encoder: full viewport until a segment asks otherwise
			if (s_do_letterbox && !ws) set_vp(1); // pass resumed after an EFB copy
			if (s_tri_verts) [enc setVertexBuffer:s_vbuf[s_vbuf_idx] offset:0 atIndex:0];
		};
		if (s_tri_verts) {
			s_vbuf_idx = (s_vbuf_idx + 1) % 3;
			// Grow to the frame's actual need rather than sizeof(s_tris). The
			// capture array is BSS and costs only the pages it touches, but these
			// are three live MTLBuffers — sizing them at full capacity would
			// reserve 3x64 MiB to carry a measured peak of ~265k verts (~17 MiB).
			// Rounded up so a slowly-climbing frame does not reallocate at every
			// present.
			size_t need = (size_t)s_tri_verts * 16 * sizeof(float);
			need = (need + (4u << 20) - 1) & ~(size_t)((4u << 20) - 1);
			if (!s_vbuf[s_vbuf_idx] || [s_vbuf[s_vbuf_idx] length] < need)
				s_vbuf[s_vbuf_idx] = [s_device newBufferWithLength:need options:MTLResourceStorageModeShared];
			memcpy([s_vbuf[s_vbuf_idx] contents], s_tris, (size_t)s_tri_verts * 16 * sizeof(float));
			[enc setVertexBuffer:s_vbuf[s_vbuf_idx] offset:0 atIndex:0];
			for (int i = 0; i < s_draw_nsegs; i++) {
				while (next_ev < s_ncopy_events && s_copy_events[next_ev].after_seg <= i)
					resolve_copy(next_ev++);
				int t = s_draw_segs[i].tex;
				if (t == -2) continue; // EFB-copy texture we can't populate; skip (scene shows through)
				int blendcode = (s_draw_segs[i].depth >> 2) & 0x7f;
				int atest = (s_draw_segs[i].depth >> 9) & 1;
				if (ws) {
					const bool rtt = s_do_letterbox && seg_is_rtt(i);
					set_vp(rtt ? 1 : 0);
					float xs = 1.0f;
					if (!rtt && ((s_draw_segs[i].depth >> 12) & 1)) {
						// 2D batch: keep its 4:3 shape unless it covers the full width.
						float lo = 1e9f, hi = -1e9f;
						const float* vv = &s_tris[(size_t)s_draw_segs[i].start * 16];
						for (int k = 0; k < s_draw_segs[i].count; k++, vv += 16) {
							const float w = vv[3] != 0.0f ? vv[3] : 1.0f, x = vv[0] / w;
							if (x < lo) lo = x;
							if (x > hi) hi = x;
						}
						if (!(lo <= -0.97f && hi >= 0.97f)) xs = 1.0f / s_ws_scale;
					}
					[enc setVertexBytes:&xs length:sizeof(xs) atIndex:1];
				}
				static int s_nodepth = -1;
				if (s_nodepth < 0) s_nodepth = getenv("PC_NO_DEPTH") ? 1 : 0;
				// PC_NO_DEPTH: force Always-compare (keep write bit) to test whether
				// the depth test is what's hiding the world geometry.
				[enc setDepthStencilState:s_ds[s_nodepth ? (s_draw_segs[i].depth & 2) : (s_draw_segs[i].depth & 3)]];
				static int s_notex = -1;
				if (s_notex < 0) s_notex = getenv("PC_NO_TEX") ? 1 : 0;
				// The TEV program is the fragment uniform; bind each sampling
				// stage's texture at its stage index. A handle that fails
				// validation degrades to the neutral white texel rather than
				// sampling an unbound slot.
				PcTevProg* P = &s_draw_segs[i].prog;
				for (int st = 0; st < P->nstages; st++) {
					if (P->tex[st][0] != PC_TEVTEX_SAMPLE) continue;
					int h = P->misc[st][3];
					if (s_notex || h < 0 || h >= s_ntextures || !s_textures[h]) {
						P->tex[st][0] = PC_TEVTEX_WHITE;
						continue;
					}
					[enc setFragmentTexture:s_textures[h] atIndex:(NSUInteger)st];
					[enc setFragmentSamplerState:s_tex_samplers[h] atIndex:(NSUInteger)st];
				}
				// GX cull mode (depth word bits 10-11): NONE=0 FRONT=1 BACK=2 ALL=3.
				// Verified empirically: GX front faces arrive CLOCKWISE in Metal
				// through our clip-space handoff (CCW culled half the title scene).
				int cull = (s_draw_segs[i].depth >> 10) & 3;
				static int s_nocull = -1;
				if (s_nocull < 0) s_nocull = getenv("PC_NO_CULL") ? 1 : 0;
				if (s_nocull) cull = 0; // A/B toggle: attribute missing geometry to culling
				[enc setFrontFacingWinding:MTLWindingClockwise];
				[enc setCullMode:(cull == 2)   ? MTLCullModeBack
				                : (cull == 1) ? MTLCullModeFront
				                              : MTLCullModeNone]; // ALL not expressible; draw
				[enc setRenderPipelineState:pc_get_pso(blendcode, atest)];
				[enc setFragmentBytes:P length:sizeof(PcTevProg) atIndex:0];
				[enc drawPrimitives:MTLPrimitiveTypeTriangle
				         vertexStart:(NSUInteger)s_draw_segs[i].start
				         vertexCount:(NSUInteger)s_draw_segs[i].count];
			}
		}
		// grabs recorded after the last drawn seg
		while (next_ev < s_ncopy_events)
			resolve_copy(next_ev++);
		s_ncopy_events = 0;
		[enc endEncoding];

		if (s_lowres) {
			// Upscale the 640x480 scene into the drawable with the
			// copy-filter approximation (ups_f).
			if (!s_ups_pso) {
				MTLRenderPipelineDescriptor* ud = [MTLRenderPipelineDescriptor new];
				ud.vertexFunction = [s_lib newFunctionWithName:@"ups_v"];
				ud.fragmentFunction = [s_lib newFunctionWithName:@"ups_f"];
				ud.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
				NSError* uerr = nil;
				s_ups_pso = [s_device newRenderPipelineStateWithDescriptor:ud error:&uerr];
				if (!s_ups_pso)
					fprintf(stderr, "[PC] upscale pso failed: %s\n", [[uerr localizedDescription] UTF8String]);
			}
			if (s_ups_pso) {
				MTLRenderPassDescriptor* up = [MTLRenderPassDescriptor renderPassDescriptor];
				up.colorAttachments[0].texture = drawable.texture;
				up.colorAttachments[0].loadAction = MTLLoadActionDontCare;
				up.colorAttachments[0].storeAction = MTLStoreActionStore;
				id<MTLRenderCommandEncoder> ue = [cmd renderCommandEncoderWithDescriptor:up];
				[ue setRenderPipelineState:s_ups_pso];
				[ue setFragmentTexture:s_efb_tex atIndex:0];
				[ue setFragmentSamplerState:pc_sampler(0, 0, 0) atIndex:0];
				[ue drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
				[ue endEncoding];
			}
		}

		// ---- touch overlay -------------------------------------------------
		// Drawn last, straight onto the drawable with the FULL viewport (not
		// s_letterbox): the controls live in the letterbox bars, which are
		// outside the scene's viewport by definition. Alpha-blended, no depth.
		// The DRAWABLE's aspect, not the scene target's: in lowres mode the
		// scene is 640x480 upscaled and the overlay belongs to the screen.
		pc_touch_overlay_draw(cmd, drawable.texture,
		                      (float)drawable.texture.width / (float)drawable.texture.height);

		// PC_DUMP_FRAMES="600,1200": blit those frames to PNG (self-serve eyes)
		static int s_frame_no;
		s_frame_no++;
		// Set by the PC_DUMP_TICKS path below so the file is named by tick.
		unsigned int s_dump_tick = ~0u;
		bool dump = false;
		if (capture) dump = true; // F9 on-demand grab (snapshot taken above)
		if (const char* df = getenv("PC_DUMP_FRAMES")) {
			char want[16];
			snprintf(want, sizeof(want), "%d", s_frame_no);
			const char* p = df;
			size_t wl = strlen(want);
			while ((p = strstr(p, want)) != nullptr) {
				bool lok = (p == df) || p[-1] == ',';
				bool rok = p[wl] == '\0' || p[wl] == ',';
				if (lok && rok) { dump = true; break; }
				p++;
			}
		}
		// PC_DUMP_TICKS="1200,1600": the same thing indexed on the SIMULATION
		// tick (PADRead calls) instead of on presents.
		//
		// PC_DUMP_FRAMES counts presents, and presents are suppressed during
		// asset loads — so a faster build presents a different number of frames
		// getting to the same point in the game, and "frame 1200" is a
		// different moment in each build. That silently invalidates any A/B
		// across builds: an -O0 vs -O2 comparison of a replayed session showed
		// 45-85% differing pixels purely from this, with both runs playing the
		// identical recorded input. Ticks do not have that problem — the game
		// reads the pad exactly once per update, so tick N is the same moment
		// in every build.
		//
		// Dumps on the first present at or after each target, once each, so a
		// tick that presents twice (or not at all) still yields exactly one
		// image per requested tick.
		if (const char* dt = getenv("PC_DUMP_TICKS")) {
			enum { kMaxTargets = 32 };
			static unsigned int targets[kMaxTargets];
			static bool done[kMaxTargets];
			static int ntargets = -1;
			if (ntargets < 0) {
				ntargets = 0;
				for (const char* p = dt; *p && ntargets < kMaxTargets;) {
					while (*p == ',' || *p == ' ') p++;
					if (!*p) break;
					targets[ntargets++] = (unsigned int)strtoul(p, (char**)&p, 10);
				}
			}
			const unsigned int tick = pc_input_current_tick();
			for (int i = 0; i < ntargets; i++) {
				if (!done[i] && tick >= targets[i]) {
					done[i] = true;
					dump = true;
					s_dump_tick = targets[i]; // names the file below
					break;
				}
			}
		}
		id<MTLBuffer> dumpBuf = nil;
		NSUInteger dw = 0, dh = 0;
		if (dump) {
			dw = drawable.texture.width;
			dh = drawable.texture.height;
			dumpBuf = [s_device newBufferWithLength:dw * dh * 4 options:MTLResourceStorageModeShared];
			id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
			[blit copyFromTexture:drawable.texture
			                  sourceSlice:0
			                  sourceLevel:0
			                 sourceOrigin:MTLOriginMake(0, 0, 0)
			                   sourceSize:MTLSizeMake(dw, dh, 1)
			                     toBuffer:dumpBuf
			            destinationOffset:0
			       destinationBytesPerRow:dw * 4
			     destinationBytesPerImage:dw * dh * 4];
			[blit endEncoding];
		}

		[cmd presentDrawable:drawable];
		[cmd commit];
		pc_tex_drain_frees(); // safe here: Metal retains textures the buffer uses
		if (docapture) {
			[cmd waitUntilCompleted];
			[[MTLCaptureManager sharedCaptureManager] stopCapture];
			fprintf(stderr, "[GPU] wrote pikmin.gputrace (in the run's working dir)\n");
		}
		if (dump) {
			[cmd waitUntilCompleted];
			char path[256];
			if (capture) snprintf(path, sizeof(path), "pc_capture.png");
			else if (s_dump_tick != ~0u) snprintf(path, sizeof(path), "pc_tick_%06u.png", s_dump_tick);
			else snprintf(path, sizeof(path), "pc_frame_%05d.png", s_frame_no);
			CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
			CGContextRef ctx = CGBitmapContextCreate([dumpBuf contents], dw, dh, 8, dw * 4, cs,
			                                         kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst);
			CGImageRef img = CGBitmapContextCreateImage(ctx);
			NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
			CGImageDestinationRef dst = CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, nullptr);
			if (dst) {
				CGImageDestinationAddImage(dst, img, nullptr);
				CGImageDestinationFinalize(dst);
				fprintf(stderr, "[PC] dumped %s (%lux%lu)\n", path, (unsigned long)dw, (unsigned long)dh);
			}
			if (dst) CFRelease(dst);
			CGImageRelease(img);
			CGContextRelease(ctx);
			CGColorSpaceRelease(cs);
		}
		if (capture) s_capture_req = 0; // consumed; segs already dumped this frame
		// PC_VTXDIAG on THIS side as well as the shim's, so each side checks the
		// same demand against ITS OWN capacity — the thing that was missing, and
		// the reason two rounds of measurement read "ok" while this half dropped.
		// Two distinct signals, verified by lowering the cap and watching them:
		//   * "OVERFLOWING" here  = the CONSUMER is dropping segments.
		//   * these peaks BELOW the shim's = the PRODUCER already dropped, since
		//     the shim replays its segments here 1:1 and only overflow makes it
		//     hand over fewer than it captured.
		// Equal numbers with neither tagged is the healthy state.
		{
			static int vtxdiag = -1;
			if (vtxdiag < 0) vtxdiag = getenv("PC_VTXDIAG") ? 1 : 0;
			if (vtxdiag) {
				static int peakv = 0, peaks = 0;
				if (s_want_verts > peakv) {
					peakv = s_want_verts;
					fprintf(stderr, "[PC] peak submitted=%d verts (capacity %d, %s)\n", peakv, PC_MAX_FRAME_VERTS,
					        peakv > PC_MAX_FRAME_VERTS ? "OVERFLOWING" : "ok");
				}
				if (s_want_segs > peaks) {
					peaks = s_want_segs;
					fprintf(stderr, "[PC] peak submitted=%d segs (capacity %d, %s)\n", peaks, PC_MAX_SEGS,
					        peaks > PC_MAX_SEGS ? "OVERFLOWING" : "ok");
				}
			}
		}
		s_want_verts = 0;
		s_want_segs = 0;
		s_tri_verts = 0;
		s_draw_nsegs = 0;
	}
}
