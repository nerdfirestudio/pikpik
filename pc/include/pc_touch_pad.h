// pc_touch_pad.h — on-screen touch controller overlay (iOS).
//
// Ported from the prior port port's pc_touch_pad.c. The finger state machine
// (floating stick, per-finger button latching, auto-hide when a real
// controller appears) is carried over as-is because it is proven; the DRAWING
// is not, because AC drew through the game's own 320x240 menu/font display
// lists and Pikmin has no equivalent layer. Here the overlay emits shape
// records and pc_touch_draw.mm rasterises them with an SDF in its own Metal
// pass.
#ifndef PC_TOUCH_PAD_H
#define PC_TOUCH_PAD_H

#ifdef __cplusplus
extern "C" {
#endif

// Stroke-font metrics, in CAP HEIGHTS. The layout needs them to size a label's
// bounding box; the shader needs them to place the pen. One definition,
// stringified into the MSL source, so the two can't drift apart.
#define PC_TOUCH_GLYPH_ADV 0.85f // pen advance per glyph
#define PC_TOUCH_GLYPH_INK 0.74f // inked width of one glyph, stroke included
#define PC_TOUCH_GLYPH_MAX 8     // glyphs per label
// Font coverage, in the shader's table order. Six letters is all the pad
// needs, plus the two extra that spell START.
#define PC_TOUCH_GLYPH_SET "ABLRSTXYZ"

// One drawable primitive: a rounded box (a circle or capsule when the corner
// radius saturates), optionally outlined, optionally carrying a label.
//
// Everything is in the same UNIT space the layout and hit-testing use: y in
// [0,1] top to bottom, x in [0, aspect]. The space is isotropic — one unit of x
// and one unit of y are the same number of pixels — so a circle stays a circle
// and `rot` means what it says. Keeping one space for layout, hit-testing and
// drawing is what stops a drawn button and its touch zone drifting apart.
typedef struct {
	float cx, cy;  // centre
	float hw, hh;  // half extents, before rotation
	float radius;  // corner radius; >= min(hw,hh) gives a capsule/circle
	float rot;     // radians, counter-clockwise as seen on screen
	float fill[4]; // rgba, straight alpha
	float edge[4]; // outline rgba
	float edgew;   // outline thickness (0 = no outline)
	float lh;      // label cap height (0 = no label)
	float lcol[4]; // label rgba
	// Indices into PC_TOUCH_GLYPH_SET, terminated by -1.
	signed char glyph[PC_TOUCH_GLYPH_MAX];
} PcTouchShape;

// Drawable aspect (width/height). Call once per frame before events/draw:
// the layout is anchored to the right and bottom edges, so it has to know
// where those are.
void pc_touch_pad_set_aspect(float aspect);

// A connected controller hides the overlay (and drops any held touches).
void pc_touch_pad_set_controller(int present);

// Advance the idle timer by `dt` seconds; call once per frame. Untouched, the
// overlay settles to a dimmer resting level so it stops competing with the
// game; any touch brings it straight back. Not calling this simply leaves it at
// full opacity.
void pc_touch_pad_advance(float dt);

// Feed every SDL_FINGER* event. Takes void* so this header stays SDL-free.
void pc_touch_pad_handle_event(const void* sdl_event);

// OR the overlay's state into a pad sample. trigL/trigR get the analog value for
// L/R, which the camera's partial-hold path reads separately from the bit.
void pc_touch_pad_apply(unsigned short* buttons, signed char* stickX, signed char* stickY, signed char* subX,
                        signed char* subY, unsigned char* trigL, unsigned char* trigR);

// Fills out[] with the shapes to draw, back to front; returns how many. 0 when
// hidden. 32 is always enough for the current layout (22 shapes with R, Z and the C-stick).
int pc_touch_pad_get_shapes(PcTouchShape* out, int max);

#ifdef __cplusplus
}
#endif

#endif
