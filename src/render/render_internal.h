/* Private renderer internals shared between the render sources and the window
 * layer. Not part of the public API. */
#ifndef AFNDLE_RENDER_RENDER_INTERNAL_H
#define AFNDLE_RENDER_RENDER_INTERNAL_H

#include <SDL.h>
#include <SDL_ttf.h>

#include "afndle/render/render.h"

/* Sprite sort nudge. 0 means "sort by the sprite's own Y". */
#define AF_SORT_AUTO 0.0f

AfRenderer* af_r2d_create(SDL_Renderer* sdl, int width, int height, float scale);
void        af_r2d_destroy(AfRenderer* r);
void        af_r2d_set_size(AfRenderer* r, int w, int h, float scale);
SDL_Renderer* af_r2d_sdl(AfRenderer* r);
/** The renderer currently inside af_r2d_begin(), for texture helpers. */
AfRenderer* af_r2d_current(void);

/* Screen-space scope: swaps in an identity camera so world draws land in
 * pixel coordinates. Save the camera on entry, hand it back on exit. */
void af_r2d_screen_space_begin(AfRenderer* r, AfCamera2D* out_saved);
void af_r2d_screen_space_end(AfRenderer* r, const AfCamera2D* saved);

/* Submits a glyph quad through the same batcher as everything else. */
void af_r2d_push_glyph_quad(AfRenderer* r, AfTexture* tex, AfVec2 pos,
                            AfVec2 size, AfRect uv, AfColor tint);

/* Packs a colour into the ABGR8888 word SDL vertices use. */
AF_INLINE Uint32 af_r2d_pack_color(AfColor c) {
    Uint32 r = (Uint32)(af_clamp01(c.r) * 255.0f + 0.5f);
    Uint32 g = (Uint32)(af_clamp01(c.g) * 255.0f + 0.5f);
    Uint32 b = (Uint32)(af_clamp01(c.b) * 255.0f + 0.5f);
    Uint32 a = (Uint32)(af_clamp01(c.a) * 255.0f + 0.5f);
    return (a << 24) | (r << 16) | (g << 8) | b;
}

#endif /* AFNDLE_RENDER_RENDER_INTERNAL_H */
