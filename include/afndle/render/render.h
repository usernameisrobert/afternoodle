/* The 2D renderer.
 *
 * Two paths, on purpose:
 *   - Sprites are *recorded* into a buffer and flushed sorted (layer, then Y,
 *     then texture). That gives correct alpha for the normal game path without
 *     asking the user to think about draw order.
 *   - Everything else (shapes, UI, debug) draws *immediately* in call order.
 *
 * Under the hood both go through the same batcher: quads sharing a texture and
 * a clip rect are accumulated into one geometry buffer and submitted in a
 * single call, so a screen of a thousand sprites is a handful of draw calls.
 */
#ifndef AFNDLE_RENDER_RENDER_H
#define AFNDLE_RENDER_RENDER_H

#include "afndle/core/afconfig.h"
#include "afndle/core/math.h"
#include "afndle/core/str.h"
#include "afndle/render/font.h"
#include "afndle/render/texture.h"

/* ================================================================= camera */

typedef struct {
    AfVec2  position;   /* world point at the centre of the view */
    AfVec2  offset;     /* added after position; shake goes here */
    float   rotation;   /* radians, clockwise on screen */
    float   zoom;       /* 1 = 1 world unit per world unit */
    AfVec2  size;       /* view size in pixels; set by the renderer each frame */
    AfRect  bounds;     /* if non-empty, the camera is clamped inside this */
    int     has_bounds;
} AfCamera2D;

AF_API AfCamera2D af_camera_default(void);
AF_API AfCamera2D af_camera_at(AfVec2 position, float zoom);
/** view -> world for a point under this camera. */
AF_API AfVec2     af_camera_to_world(AfCamera2D cam, AfVec2 screen_point);
AF_API AfVec2     af_camera_to_screen(AfCamera2D cam, AfVec2 world_point);
/** Snaps position so the visible rect stays inside `bounds`. */
AF_API AfCamera2D af_camera_clamp(AfCamera2D cam, AfRect bounds);
/** Smooth follow with an exponential half-life, clamped to bounds. */
AF_API AfCamera2D af_camera_follow(AfCamera2D cam, AfVec2 target, float half_life,
                                   float dt, AfRect bounds);

/* =============================================================== renderer */

#define AF_MAX_CLIP_DEPTH 32

typedef struct {
    int    draw_calls;   /* geometry submissions */
    int    quads;
    int    triangles;
    int    batches;      /* texture/clip switches */
    int    vertices;
    int    textures;
    int    sprite_count;
    float  cpu_ms;
} AfRenderStats;

AF_API void        af_r2d_begin(AfRenderer* r, AfColor clear_color);
AF_API void        af_r2d_end(AfRenderer* r);
AF_API void        af_r2d_clear(AfRenderer* r, AfColor c);
AF_API AfVec2      af_r2d_screen_size(AfRenderer* r);
AF_API float       af_r2d_render_scale(AfRenderer* r);
AF_API AfRenderer* af_r2d_of_window(void* window);
/* SDL2 textures belong to the renderer that created them, so the engine keeps
 * one active: the renderer of the frame being drawn, else the newest one. A
 * second window calls this before creating or loading its own textures. */
AF_API void        af_r2d_set_texture_context(AfRenderer* r);
AF_API AfRenderer* af_r2d_texture_context(void);

AF_API void        af_r2d_set_camera(AfRenderer* r, AfCamera2D cam);
AF_API AfCamera2D  af_r2d_get_camera(AfRenderer* r);
/** Current view -> world, taking render scale into account. */
AF_API AfVec2      af_r2d_screen_to_world(AfRenderer* r, AfVec2 screen);
AF_API AfVec2      af_r2d_world_to_screen(AfRenderer* r, AfVec2 world);
AF_API AfRect      af_r2d_screen_rect_to_world(AfRenderer* r, AfRect screen);
AF_API AfRect      af_r2d_visible_world_rect(AfRenderer* r);

AF_API void        af_r2d_push_clip(AfRenderer* r, AfRect rect);
AF_API void        af_r2d_pop_clip(AfRenderer* r);
AF_API AfRect      af_r2d_clip(AfRenderer* r);

/* --------------------------------------------------------------- sprites */
/* Recorded, then drawn sorted by (layer, sort_key, texture). */
AF_API void af_r2d_sprite(AfRenderer* r, AfTexture* tex, AfRect dst, AfRect uv,
                          AfColor tint, float rotation, AfVec2 pivot, int layer,
                          float sort_key);
AF_API void af_r2d_sprite_layer(AfRenderer* r, AfTexture* tex, AfRect dst,
                                AfRect uv, AfColor tint, int layer,
                                float sort_key);
/** Whole texture into `dst` with no rotation. */
AF_API void af_r2d_sprite_simple(AfRenderer* r, AfTexture* tex, AfRect dst,
                                 AfColor tint, int layer);
/** Centred on `center`, sized from the texture. */
AF_API void af_r2d_sprite_centered(AfRenderer* r, AfTexture* tex, AfVec2 center,
                                   float scale, AfColor tint, float rotation,
                                   int layer);
/** Flips a recorded sprite's UVs. */
AF_API void af_r2d_sprite_flipped(AfRenderer* r, int flip_x, int flip_y);
/** Sorts and draws everything recorded so far, then clears the buffer. */
AF_API void af_r2d_flush_sprites(AfRenderer* r);
AF_API void af_r2d_clear_sprites(AfRenderer* r);
AF_API int  af_r2d_sprite_count(AfRenderer* r);

/* ---------------------------------------------------------- direct quads */
/* Immediate: no sorting, no recording. */
AF_API void af_r2d_quad(AfRenderer* r, AfTexture* tex, AfVec2 center,
                        AfVec2 half_size, float rotation, AfRect uv, AfColor tint);
/** Axis-aligned textured rect, unrotated. The bread-and-butter UI call. */
AF_API void af_r2d_tex_rect(AfRenderer* r, AfTexture* tex, AfRect dst, AfRect uv,
                            AfColor tint);
/** Solid fill using the current camera. */
AF_API void af_r2d_fill(AfRenderer* r, AfRect rect, AfColor c);
/** Solid fill in screen pixels, ignoring the camera. */
AF_API void af_r2d_ui_fill(AfRenderer* r, AfRect rect, AfColor c);
/** Textured rect in screen pixels, ignoring the camera. */
AF_API void af_r2d_ui_tex(AfRenderer* r, AfTexture* tex, AfRect dst, AfRect uv,
                          AfColor tint);
AF_API void af_r2d_tri(AfRenderer* r, AfVec2 a, AfVec2 b, AfVec2 c, AfColor col);
AF_API void af_r2d_line(AfRenderer* r, AfVec2 a, AfVec2 b, float thickness,
                        AfColor c);
AF_API void af_r2d_line_aa(AfRenderer* r, AfVec2 a, AfVec2 b, float thickness,
                           AfColor c);
AF_API void af_r2d_rect_outline(AfRenderer* r, AfRect rect, float thickness,
                                AfColor c);
AF_API void af_r2d_ui_rect_outline(AfRenderer* r, AfRect rect, float thickness,
                                   AfColor c);
AF_API void af_r2d_circle(AfRenderer* r, AfVec2 center, float radius, AfColor c,
                          int segments);
AF_API void af_r2d_circle_outline(AfRenderer* r, AfVec2 center, float radius,
                                  float thickness, AfColor c, int segments);
AF_API void af_r2d_poly(AfRenderer* r, AfVec2* pts, int count, AfColor c);
AF_API void af_r2d_poly_outline(AfRenderer* r, AfVec2* pts, int count,
                                float thickness, AfColor c);
/** Axis-aligned wireframe box, handy for debugging bounds. */
AF_API void af_r2d_aabb(AfRenderer* r, AfBounds b, AfColor c);

/* ----------------------------------------------------------------- text */
AF_API void  af_r2d_text(AfRenderer* r, AfFont* font, AfVec2 pos, const char* text,
                         AfColor c, float size);
/** Text with alignment relative to `pos`. */
typedef enum {
    AF_ALIGN_LEFT = 0, AF_ALIGN_CENTER, AF_ALIGN_RIGHT,
    AF_ALIGN_TOP, AF_ALIGN_MIDDLE, AF_ALIGN_BOTTOM
} AfAlign;
AF_API void  af_r2d_text_aligned(AfRenderer* r, AfFont* font, AfVec2 pos,
                                 AfAlign h, AfAlign v, const char* text,
                                 AfColor c, float size);
/** Screen-space text (ignores the camera). */
AF_API void  af_r2d_ui_text(AfRenderer* r, AfFont* font, AfVec2 pos,
                            const char* text, AfColor c, float size);
AF_API void  af_r2d_ui_text_aligned(AfRenderer* r, AfFont* font, AfVec2 pos,
                                    AfAlign h, AfAlign v, const char* text,
                                    AfColor c, float size);
/** Outlined text, for HUD over busy backgrounds. */
AF_API void  af_r2d_text_outline(AfRenderer* r, AfFont* font, AfVec2 pos,
                                 const char* text, AfColor c, AfColor outline,
                                 float size, float outline_width);
AF_API float af_r2d_text_width(AfRenderer* r, AfFont* font, const char* text,
                               float size);
AF_API float af_r2d_text_height(AfRenderer* r, AfFont* font, float size);
/** Draws `text` scaled to exactly `width`; `max_width` clamps it. */
AF_API void  af_r2d_text_sized(AfRenderer* r, AfFont* font, AfVec2 pos,
                               const char* text, float width, float max_height,
                               AfColor c);

/* ----------------------------------------------------------------- stats */
AF_API AfRenderStats af_r2d_stats(AfRenderer* r);
AF_API void          af_r2d_reset_stats(AfRenderer* r);
/** Forces every batch to flush now. */
AF_API void          af_r2d_flush(AfRenderer* r);

#endif /* AFNDLE_RENDER_RENDER_H */
