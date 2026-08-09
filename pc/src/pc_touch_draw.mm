// pc_touch_draw.mm — Metal rasteriser for the touch overlay (iOS).
//
// One instanced 4-vertex draw for the whole overlay. Each instance is a
// ROUNDED BOX evaluated as a signed distance field, which is the single trick
// the rest of this file hangs off:
//
//  - a circle is a rounded box whose radius saturates, and a capsule is the
//    same with unequal extents, so A, B, the stick and the GameCube's rotated
//    X/Y kidneys are all one shader with no branches;
//  - the outline is a second threshold on the same distance, so it costs
//    nothing and can never disagree with the fill about where the edge is;
//  - the distance is in the layout's own unit space, so `fwidth` gives exact
//    per-pixel antialiasing at any drawable size without knowing the size.
//
// Labels are a STROKE font (see kSeg): every letter is a handful of line
// segments, and a glyph's coverage is the distance to the nearest segment
// thresholded at the stroke half-width. Six letters plus the two that spell
// START is far too little to justify a glyph atlas, and strokes stay smooth at
// the ~30px cap height this draws at, which a 5x7 bitmap would not.
#import "pc_touch_draw.h"

#import "pc_touch_pad.h"

#include <stdio.h>
#include <string.h>

#define TD_MAX_SHAPES 32

// Mirrors TDInst in the shader. All members are float4-sized and float4-aligned
// so the C layout and the MSL layout are the same bytes.
typedef struct __attribute__((aligned(16))) {
	float geo[4];  // cx, cy, hw, hh
	float sty[4];  // corner radius, outline width, rotation, label cap height
	float fill[4]; // rgba
	float edge[4]; // rgba
	float lcol[4]; // label rgba
	float g0[4];   // glyph ids 0..3, -1 = none
	float g1[4];   // glyph ids 4..7
} TDInst;

#define TD_STR2(x) #x
#define TD_STR(x)  TD_STR2(x)

// The font metrics come from pc_touch_pad.h so the layout (which sizes a
// label's box) and the shader (which places the pen) cannot drift apart.
static const char* kTouchShader =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "constant float ADV = " TD_STR(PC_TOUCH_GLYPH_ADV) ";\n"
    "constant float INK = " TD_STR(PC_TOUCH_GLYPH_INK) ";\n"
    // Stroke half-width, and the matching left side bearing that puts the ink
    // edge at x = -STROKE. INK = 0.60 (glyph box) + 2 * STROKE.
    "constant float STROKE = 0.070;\n"
    "constant float BEAR   = 0.070;\n"
    "struct TDInst {\n"
    "  float4 geo; float4 sty; float4 fill; float4 edge; float4 lcol; float4 g0; float4 g1;\n"
    "};\n"
    // Stroke font. Each glyph is a run of segments in an em box: x spans
    // 0..0.60, y spans 0 (cap top) to 1 (baseline), y DOWN to match unit
    // space. Runs are in PC_TOUCH_GLYPH_SET order: A B L R S T X Y.
    "constant float4 kSeg[40] = {\n"
    "  {0.00,1.00,0.30,0.00},{0.30,0.00,0.60,1.00},{0.10,0.68,0.50,0.68},\n" // A
    "  {0.06,0.00,0.06,1.00},{0.06,0.00,0.36,0.00},{0.36,0.00,0.52,0.12},\n" // B
    "  {0.52,0.12,0.52,0.34},{0.52,0.34,0.36,0.47},{0.36,0.47,0.06,0.47},\n"
    "  {0.36,0.47,0.55,0.61},{0.55,0.61,0.55,0.86},{0.55,0.86,0.36,1.00},\n"
    "  {0.36,1.00,0.06,1.00},\n"
    "  {0.08,0.00,0.08,1.00},{0.08,1.00,0.55,1.00},\n" // L
    "  {0.06,0.00,0.06,1.00},{0.06,0.00,0.36,0.00},{0.36,0.00,0.52,0.13},\n" // R
    "  {0.52,0.13,0.52,0.35},{0.52,0.35,0.36,0.48},{0.36,0.48,0.06,0.48},\n"
    "  {0.30,0.48,0.58,1.00},\n"
    "  {0.56,0.14,0.44,0.02},{0.44,0.02,0.19,0.02},{0.19,0.02,0.06,0.14},\n" // S
    "  {0.06,0.14,0.06,0.34},{0.06,0.34,0.19,0.45},{0.19,0.45,0.42,0.53},\n"
    "  {0.42,0.53,0.55,0.65},{0.55,0.65,0.55,0.86},{0.55,0.86,0.42,0.98},\n"
    "  {0.42,0.98,0.15,0.98},{0.15,0.98,0.04,0.86},\n"
    "  {0.00,0.02,0.60,0.02},{0.30,0.02,0.30,1.00},\n" // T
    "  {0.03,0.00,0.57,1.00},{0.57,0.00,0.03,1.00},\n" // X
    "  {0.03,0.00,0.30,0.52},{0.57,0.00,0.30,0.52},{0.30,0.52,0.30,1.00},\n" // Y
    "};\n"
    "constant int kFirst[8] = { 0, 3, 13, 15, 22, 33, 35, 37 };\n"
    "constant int kCount[8] = { 3, 10,  2,  7, 11,  2,  2,  3 };\n"
    "static float sd_box(float2 p, float2 b, float r) {\n"
    "  float2 q = abs(p) - b + r;\n"
    "  return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
    "}\n"
    "static float sd_seg(float2 p, float2 a, float2 b) {\n"
    "  float2 pa = p - a, ba = b - a;\n"
    "  float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);\n"
    "  return length(pa - ba * h);\n"
    "}\n"
    "struct TDVOut { float4 pos [[position]]; float2 local; uint iid [[flat]]; };\n"
    "vertex TDVOut td_v(uint vid [[vertex_id]], uint iid [[instance_id]],\n"
    "                   const device TDInst* sh [[buffer(0)]],\n"
    "                   constant float& aspect [[buffer(1)]]) {\n"
    "  TDInst s = sh[iid];\n"
    // Grow past the outline (which sits inside the edge) by enough for the
    // antialias feather; a few thousandths of screen height is plenty.
    "  float m = s.sty.y + 0.005;\n"
    "  float2 h = s.geo.zw + m;\n"
    "  float2 c = float2((vid & 1) ? h.x : -h.x, (vid & 2) ? h.y : -h.y);\n"
    // rot turns local +x counter-clockwise ON SCREEN, and unit y points down.
    "  float ca = cos(s.sty.z), sa = sin(s.sty.z);\n"
    "  float2 u = float2(c.x * ca + c.y * sa, -c.x * sa + c.y * ca) + s.geo.xy;\n"
    "  TDVOut o;\n"
    "  o.pos = float4((u.x / aspect) * 2.0 - 1.0, 1.0 - u.y * 2.0, 0.0, 1.0);\n"
    "  o.local = c;\n"
    "  o.iid = iid;\n"
    "  return o;\n"
    "}\n"
    "fragment float4 td_f(TDVOut in [[stage_in]], const device TDInst* sh [[buffer(0)]]) {\n"
    "  TDInst s = sh[in.iid];\n"
    "  float2 b = s.geo.zw;\n"
    "  float d = sd_box(in.local, b, clamp(s.sty.x, 0.0, min(b.x, b.y)));\n"
    "  float aa = max(fwidth(d), 1e-5);\n"
    "  float4 col = s.fill;\n"
    "  if (s.sty.y > 0.0) col = mix(s.fill, s.edge, smoothstep(-aa, aa, d + s.sty.y));\n"
    "  col.a *= smoothstep(aa, -aa, d);\n"
    "  float lh = s.sty.w;\n"
    "  if (lh > 0.0) {\n"
    "    float ids[8] = { s.g0.x, s.g0.y, s.g0.z, s.g0.w, s.g1.x, s.g1.y, s.g1.z, s.g1.w };\n"
    "    int n = 0; while (n < 8 && ids[n] >= 0.0) n++;\n"
    "    if (n > 0) {\n"
    "      float2 e = in.local / lh;\n" // em units, centred on the label box
    "      float x0 = -0.5 * (float(n - 1) * ADV + INK) + BEAR;\n"
    "      float dmin = 1e9;\n"
    "      for (int i = 0; i < n; i++) {\n"
    "        float2 g = e - float2(x0 + float(i) * ADV, -0.5);\n"
    "        int gi = int(ids[i]);\n"
    "        int first = kFirst[gi], cnt = kCount[gi];\n"
    "        for (int k = 0; k < cnt; k++) {\n"
    "          float4 sg = kSeg[first + k];\n"
    "          dmin = min(dmin, sd_seg(g, sg.xy, sg.zw));\n"
    "        }\n"
    "      }\n"
    "      float ga = smoothstep(aa, -aa, (dmin - STROKE) * lh) * s.lcol.a;\n"
    // Straight-alpha "over": the letter sits on the button face, and the pair
    // is then blended onto the scene as one colour.
    "      float outA = ga + col.a * (1.0 - ga);\n"
    "      float3 rgb = (outA > 1e-5)\n"
    "                 ? (s.lcol.rgb * ga + col.rgb * col.a * (1.0 - ga)) / outA : col.rgb;\n"
    "      col = float4(rgb, outA);\n"
    "    }\n"
    "  }\n"
    "  return col;\n"
    "}\n";

static id<MTLRenderPipelineState> s_pso;
static MTLPixelFormat s_pso_fmt = MTLPixelFormatInvalid;
static int s_pso_failed;

static id<MTLRenderPipelineState> td_pipeline(id<MTLDevice> dev, MTLPixelFormat fmt)
{
	if (s_pso && s_pso_fmt == fmt) {
		return s_pso;
	}
	if (s_pso_failed) {
		return nil;
	}
	NSError* err       = nil;
	id<MTLLibrary> lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:kTouchShader]
	                                       options:nil
	                                         error:&err];
	if (!lib) {
		fprintf(stderr, "[PC] touch overlay shader failed: %s\n", [[err localizedDescription] UTF8String]);
		s_pso_failed = 1;
		return nil;
	}
	MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
	d.vertexFunction               = [lib newFunctionWithName:@"td_v"];
	d.fragmentFunction             = [lib newFunctionWithName:@"td_f"];
	d.colorAttachments[0].pixelFormat                  = fmt;
	d.colorAttachments[0].blendingEnabled              = YES;
	d.colorAttachments[0].sourceRGBBlendFactor         = MTLBlendFactorSourceAlpha;
	d.colorAttachments[0].destinationRGBBlendFactor    = MTLBlendFactorOneMinusSourceAlpha;
	d.colorAttachments[0].sourceAlphaBlendFactor       = MTLBlendFactorSourceAlpha;
	d.colorAttachments[0].destinationAlphaBlendFactor  = MTLBlendFactorOneMinusSourceAlpha;
	s_pso = [dev newRenderPipelineStateWithDescriptor:d error:&err];
	if (!s_pso) {
		fprintf(stderr, "[PC] touch overlay pso failed: %s\n", [[err localizedDescription] UTF8String]);
		s_pso_failed = 1;
		return nil;
	}
	s_pso_fmt = fmt;
	return s_pso;
}

static void td_pack(TDInst* d, const PcTouchShape* s)
{
	d->geo[0] = s->cx;
	d->geo[1] = s->cy;
	d->geo[2] = s->hw;
	d->geo[3] = s->hh;
	d->sty[0] = s->radius;
	d->sty[1] = s->edgew;
	d->sty[2] = s->rot;
	d->sty[3] = s->lh;
	memcpy(d->fill, s->fill, sizeof(d->fill));
	memcpy(d->edge, s->edge, sizeof(d->edge));
	memcpy(d->lcol, s->lcol, sizeof(d->lcol));
	for (int i = 0; i < 4; i++) {
		d->g0[i] = (float)s->glyph[i];
		d->g1[i] = (float)s->glyph[i + 4];
	}
}

void pc_touch_overlay_draw(id<MTLCommandBuffer> cmd, id<MTLTexture> target, float aspect)
{
	PcTouchShape shapes[TD_MAX_SHAPES];
	const int n = pc_touch_pad_get_shapes(shapes, TD_MAX_SHAPES);
	if (n <= 0) {
		return;
	}
	id<MTLRenderPipelineState> pso = td_pipeline(target.device, target.pixelFormat);
	if (!pso) {
		return;
	}

	TDInst inst[TD_MAX_SHAPES];
	for (int i = 0; i < n; i++) {
		td_pack(&inst[i], &shapes[i]);
	}

	MTLRenderPassDescriptor* rp        = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture     = target;
	rp.colorAttachments[0].loadAction  = MTLLoadActionLoad; // keep the scene
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;

	id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
	[enc setRenderPipelineState:pso];
	[enc setVertexBytes:inst length:(size_t)n * sizeof(TDInst) atIndex:0];
	[enc setVertexBytes:&aspect length:sizeof(aspect) atIndex:1];
	[enc setFragmentBytes:inst length:(size_t)n * sizeof(TDInst) atIndex:0];
	[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip
	        vertexStart:0
	        vertexCount:4
	      instanceCount:(NSUInteger)n];
	[enc endEncoding];
}
