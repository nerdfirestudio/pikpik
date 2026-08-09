// pc_touch_draw.h — Metal rasteriser for the touch overlay (iOS).
//
// Split out of pc_window.mm because the overlay stopped being "two triangles
// and a colour": it is an SDF pass with its own shader, its own stroke font
// and its own pipeline, and none of that has anything to say to the GX
// emulation next door. Being a separate translation unit also means it can be
// driven from a standalone harness and looked at without booting the game.
#ifndef PC_TOUCH_DRAW_H
#define PC_TOUCH_DRAW_H

#ifdef __OBJC__
#import <Metal/Metal.h>

// Draw the overlay onto `target` (loaded, not cleared) on `cmd`. `aspect` is
// the TARGET's width/height — the overlay belongs to the screen, so in lowres
// mode this is the drawable's aspect, not the scene target's. Does nothing
// when the overlay is hidden.
void pc_touch_overlay_draw(id<MTLCommandBuffer> cmd, id<MTLTexture> target, float aspect);

#endif

#endif
