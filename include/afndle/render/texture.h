/* Textures: load from PNG/JPG/BMP, create from memory or procedurally.
 *
 * A texture knows its pixel size in world units (usually 1.0) so a sprite can
 * be drawn at a sensible default without the caller hardcoding numbers.
 */
#ifndef AFNDLE_RENDER_TEXTURE_H
#define AFNDLE_RENDER_TEXTURE_H

#include "afndle/core/afconfig.h"
#include "afndle/core/math.h"

typedef struct AfTexture AfTexture;
typedef struct AfRenderer AfRenderer;

typedef enum {
    AF_FILTER_NEAREST = 0,  /* crisp pixels; the default for pixel art */
    AF_FILTER_LINEAR,       /* smooth; the default for UI and photos */
    AF_FILTER_NEAREST_MIP   /* linear + auto mipmaps, for minified tiles */
} AfTextureFilter;

/** Packs channels into the ABGR8888 word af_texture_create() expects, so
 *  callers never have to reason about byte order. */
AF_INLINE uint32_t af_tex_rgba8(int r, int g, int b, int a) {
    return ((uint32_t)(a & 0xFF) << 24) | ((uint32_t)(b & 0xFF) << 16) |
           ((uint32_t)(g & 0xFF) << 8) | (uint32_t)(r & 0xFF);
}

/* Loads an image. Caches by path, so calling this twice is free. */
AF_API AfTexture* af_texture_load(const char* path);
/** Same, but forces a filter and bypasses the cache. SDL2 textures always
 *  clamp at their edge, so `repeat` is accepted for portability only. */
AF_API AfTexture* af_texture_load_ex(const char* path, AfTextureFilter filter,
                                     int repeat);
/** Every texture in the engine is ABGR8888, so `data` here must be too: four
 *  bytes per pixel in B, G, R, A order. Use af_tex_rgba8() to build the words
 *  rather than guessing at the byte order. `data` is copied; `pitch` is in
 *  bytes per row and is allowed to exceed w * 4. */
AF_API AfTexture* af_texture_create(const void* data, int w, int h, int pitch,
                                    AfTextureFilter filter);
/** A 1x1 white pixel. Always available, never freed; used for solid fills. */
AF_API AfTexture* af_texture_white(AfRenderer* r);
/** A 2x2 opaque white pixel -- the fallback when a draw has no texture, so
 *  an untextured quad still tints and stays visible. */
AF_API AfTexture* af_texture_default_sprite(void);
AF_API void       af_texture_destroy(AfTexture* t);
/** The underlying SDL_Texture, for code that has to talk to SDL directly
 *  (the font atlas does). NULL if the texture failed to create. */
AF_API void*      af_texture_sdl(AfTexture* t);
AF_API int        af_texture_valid(const AfTexture* t);

AF_API int   af_texture_width(const AfTexture* t);
AF_API int   af_texture_height(const AfTexture* t);
AF_API float af_texture_aspect(const AfTexture* t);
/** Size in world units, i.e. pixels * world_size_per_pixel. */
AF_API AfVec2 af_texture_size(const AfTexture* t);
AF_API const char* af_texture_path(const AfTexture* t);
AF_API void af_texture_set_filter(AfTexture* t, AfTextureFilter f);
/** No-op on SDL2 (textures always clamp); kept so calls stay portable. */
AF_API void af_texture_set_repeat(AfTexture* t, int repeat);
/** Replaces the contents without reallocating. */
AF_API int  af_texture_update(AfTexture* t, const void* data, int w, int h,
                              int pitch);
/** Reads a single pixel. Returns 0 (black) outside the texture. */
AF_API AfColor af_texture_get_pixel(const AfTexture* t, int x, int y);
/** Streams to a PNG on disk. Used for screenshots and the editor's image view. */
AF_API int  af_texture_save_png(AfTexture* t, const char* path);
/** Drops the path cache. Call after editing files on disk. */
AF_API void af_texture_cache_clear(void);
AF_API int  af_texture_cache_count(void);

/* --------------------------------------------------------- procedural art */
/* These are what the example games and the "add sprite" flow use so a project
 * can look like something before the user has imported any art. */
AF_API AfTexture* af_texture_solid(int w, int h, AfColor c);
AF_API AfTexture* af_texture_checker(int cell, AfColor a, AfColor b);
AF_API AfTexture* af_texture_circle(int size, AfColor fill, AfColor edge);
AF_API AfTexture* af_texture_rounded_rect(int w, int h, int radius, AfColor c);
AF_API AfTexture* af_texture_gradient_v(int w, int h, AfColor top, AfColor bottom);
AF_API AfTexture* af_texture_noise(int w, int h, AfColor a, AfColor b, uint32_t seed);
/** A one-pixel outline around `t`, inflated by `margin`. For debug overlays. */
AF_API AfTexture* af_texture_outline(AfTexture* t, AfColor c, int margin);

#endif /* AFNDLE_RENDER_TEXTURE_H */
