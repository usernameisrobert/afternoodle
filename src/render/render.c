/* Batched 2D renderer on top of SDL_Renderer's geometry API.
 *
 * Every quad becomes two triangles. Quads that share a texture and a clip rect
 * accumulate into one vertex array and go out in a single SDL_RenderGeometry
 * call, so a screen of a thousand sprites is a handful of draw calls.
 *
 * Two paths, on purpose:
 *   - Sprites are *recorded* and flushed sorted (layer, then Y, then texture).
 *     That gives correct alpha without asking the user to think about order.
 *   - Everything else draws *immediately* in call order.
 */
#include "render_internal.h"

#include <SDL.h>
#include <SDL_image.h>

#include "afndle/core/log.h"
#include "afndle/core/mem.h"
#include "afndle/core/str.h"
#include "afndle/core/time.h"
#include "afndle/platform/platform.h"
#include "afndle/platform/platform.h"
#include "afndle/render/render.h"

#define AF_SPRITE_MAX 65536
#define AF_VERT_INITIAL 2048

/* ============================================================== textures */

struct AfTexture {
    SDL_Texture*  sdl;
    SDL_Renderer* owner;      /* the renderer t->sdl is valid for */
    int   w, h;
    float world_w, world_h;
    char  path[512];
    AfTextureFilter filter;
    int   cached;
    struct AfTexture* next_hash;
};

#define AF_TEX_HASH_SIZE 127
static struct {
    AfTexture* buckets[AF_TEX_HASH_SIZE];
    int count;
} g_tex_cache;
static AfTexture *g_tex_white;
static AfTexture *g_tex_default;

/* SDL2 textures belong to the renderer that made them, so every texture needs
 * one. Callers do not pass a renderer around, so the engine keeps a context:
 * the renderer of the frame being drawn, or the most recently created one
 * before the first frame. A second window sets it with
 * af_r2d_set_texture_context() before loading its own textures. */
static AfRenderer *g_tex_ctx;

static SDL_Renderer *tex_renderer(void) {
    return g_tex_ctx ? af_r2d_sdl(g_tex_ctx) : NULL;
}

static unsigned tex_hash(const char *s, SDL_Renderer *r) {
    uintptr_t bits = (uintptr_t)r;
    uint32_t h = af_hash_cstr(s);
    h = h * 16777619u ^ (uint32_t)(bits ^ (bits >> 32));
    return (unsigned)(h % AF_TEX_HASH_SIZE);
}

static void tex_apply_filter(SDL_Texture *t, AfTextureFilter f) {
    /* SDL2 has no explicit mipmap call: a static texture with linear filtering
     * gets them generated the first time it is minified. So the "mip" mode is
     * just "linear" plus the knowledge that it will not shimmer when scaled
     * down, while NEAREST stays crisp for pixel art. */
    SDL_SetTextureScaleMode(t, f == AF_FILTER_NEAREST ? SDL_ScaleModeNearest
                                                       : SDL_ScaleModeLinear);
}

AF_INLINE Uint32 pack(AfColor c) { return af_r2d_pack_color(c); }

AfTexture *af_texture_create(const void *data, int w, int h, int pitch,
                             AfTextureFilter filter) {
    if (w <= 0 || h <= 0) return NULL;
    SDL_Renderer *r = tex_renderer();
    if (!r) {
        AF_ERROR("no renderer yet: create a window before making textures");
        return NULL;
    }
    /* Streaming so a texture can be rewritten in place later (the font atlas
     * relies on it); SDL promotes it to static usage when it can. */
    SDL_Texture *sdl = SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888,
                                         SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!sdl) {
        AF_WARN("could not create %dx%d texture: %s", w, h, SDL_GetError());
        return NULL;
    }
    if (data) SDL_UpdateTexture(sdl, NULL, data, pitch);
    tex_apply_filter(sdl, filter);
    SDL_SetTextureBlendMode(sdl, SDL_BLENDMODE_BLEND);

    AfTexture *t = (AfTexture *)af_calloc(1, sizeof(AfTexture));
    t->sdl = sdl;
    t->w = w;
    t->h = h;
    t->world_w = (float)w;
    t->world_h = (float)h;
    t->filter = filter;
    return t;
}

AfTexture *af_texture_load_ex(const char *path, AfTextureFilter filter, int repeat) {
    if (!path || !*path) return NULL;

    /* On hosts with a data dir, relative paths are resolved against it so
     * file I/O never falls through to SDL's Android internal-storage path
     * (which needs an SDLActivity). Desktop has no data dir, so nothing
     * changes there. */
    char resolved[1024];
    const char *data = af_platform_data_dir();
    if (data && *data && !af_fs_is_absolute(path)) {
        af_fs_path_join(data, path, resolved, (int)sizeof(resolved));
        path = resolved;
    }

    SDL_Renderer *cur = tex_renderer();
    unsigned h = tex_hash(path, cur);
    for (AfTexture *t = g_tex_cache.buckets[h]; t; t = t->next_hash) {
        if (strcmp(t->path, path) == 0 && t->owner == cur) {
            if (filter != AF_FILTER_NEAREST) af_texture_set_filter(t, filter);
            return t;
        }
    }

    SDL_Surface *surf = IMG_Load(path);
    if (!surf) {
        AF_WARN("could not load image '%s': %s", path, IMG_GetError());
        return NULL;
    }
    /* Normalise to ABGR8888 so every draw path can assume 4 bytes per pixel. */
    SDL_Surface *rgba =
        SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(surf);
    if (!rgba) {
        AF_WARN("could not convert image '%s': %s", path, SDL_GetError());
        return NULL;
    }

    /* Static where we can: less per-draw overhead than streaming. */
    AfTexture *t = NULL;
    if (cur) {
        SDL_Texture *st = SDL_CreateTexture(cur, rgba->format->format,
                                            SDL_TEXTUREACCESS_STATIC, rgba->w,
                                            rgba->h);
        if (st) {
            SDL_UpdateTexture(st, NULL, rgba->pixels, rgba->pitch);
            t = (AfTexture *)af_calloc(1, sizeof(AfTexture));
            t->sdl = st;
        }
    }
    if (!t) {
        t = (AfTexture *)af_calloc(1, sizeof(AfTexture));
        t->sdl = SDL_CreateTexture(cur, rgba->format->format,
                                   SDL_TEXTUREACCESS_STREAMING, rgba->w,
                                   rgba->h);
        if (!t->sdl) {
            AF_WARN("could not create texture for '%s': %s", path, SDL_GetError());
            af_free(t);
            SDL_FreeSurface(rgba);
            return NULL;
        }
        SDL_UpdateTexture(t->sdl, NULL, rgba->pixels, rgba->pitch);
    }
    t->owner = cur;
    t->w = rgba->w;
    t->h = rgba->h;
    t->world_w = (float)rgba->w;
    t->world_h = (float)rgba->h;
    t->filter = filter;
    tex_apply_filter(t->sdl, filter);
    SDL_SetTextureBlendMode(t->sdl, SDL_BLENDMODE_BLEND);
    AF_UNUSED(repeat); /* SDL2 textures always clamp; see the header. */
    af_str_cpy_max(af_str(path), t->path, (int)sizeof(t->path));
    SDL_FreeSurface(rgba);

    t->cached = 1;
    t->next_hash = g_tex_cache.buckets[h];
    g_tex_cache.buckets[h] = t;
    g_tex_cache.count++;
    return t;
}

AfTexture *af_texture_load(const char *path) {
    return af_texture_load_ex(path, AF_FILTER_NEAREST, 0);
}

int af_texture_update(AfTexture *t, const void *data, int w, int h, int pitch) {
    if (!t || !t->sdl || !data) return 0;
    SDL_UpdateTexture(t->sdl, NULL, data, pitch);
    t->w = w;
    t->h = h;
    t->world_w = (float)w;
    t->world_h = (float)h;
    return 1;
}

void af_texture_destroy(AfTexture *t) {
    if (!t) return;
    if (t->cached) return; /* owned by the cache; freed by cache_clear */
    if (t->sdl) SDL_DestroyTexture(t->sdl);
    af_free(t);
}

int af_texture_valid(const AfTexture *t) { return t && t->sdl; }
void *af_texture_sdl(AfTexture *t) { return t ? (void *)t->sdl : NULL; }
int af_texture_width(const AfTexture *t) { return t ? t->w : 0; }
int af_texture_height(const AfTexture *t) { return t ? t->h : 0; }
float af_texture_aspect(const AfTexture *t) {
    return (t && t->h > 0) ? (float)t->w / (float)t->h : 1.0f;
}
AfVec2 af_texture_size(const AfTexture *t) {
    return t ? af_v2(t->world_w, t->world_h) : af_v2s(0);
}
const char *af_texture_path(const AfTexture *t) { return t ? t->path : ""; }

void af_texture_set_filter(AfTexture *t, AfTextureFilter f) {
    if (!t || !t->sdl) return;
    t->filter = f;
    tex_apply_filter(t->sdl, f);
}

void af_texture_set_repeat(AfTexture *t, int repeat) {
    /* SDL2 has no wrap-mode control: every texture clamps to its edge. The
     * flag is accepted so calling code stays portable to an SDL3 backend. */
    if (!t || !t->sdl) return;
    AF_UNUSED(repeat);
}

/* Renders `t` into a fresh ABGR8888 surface. Needs a live renderer, so it is
 * only usable inside a frame; the editor and screenshot paths both need it. */
/* Pulls a texture's pixels back into a new surface the caller must free.
 * Readback scribbles on the renderer, so every piece of state it touches is
 * saved and put back: a caller asking for one pixel mid-frame must not lose
 * its clip, target, or blend modes. */
static SDL_Surface *texture_readback(AfTexture *t) {
    SDL_Renderer *r = tex_renderer();
    if (!t || !t->sdl || !r) return NULL;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, t->w, t->h, 32,
                                                    SDL_PIXELFORMAT_ABGR8888);
    if (!s) return NULL;

    SDL_Rect saved_clip;
    SDL_RenderGetClipRect(r, &saved_clip);
    SDL_BlendMode saved_draw_blend;
    SDL_GetRenderDrawBlendMode(r, &saved_draw_blend);
    Uint8 cr, cg, cb, ca;
    SDL_GetRenderDrawColor(r, &cr, &cg, &cb, &ca);
    SDL_Texture *saved_target = SDL_GetRenderTarget(r);
    SDL_BlendMode saved_tex_blend;
    SDL_GetTextureBlendMode(t->sdl, &saved_tex_blend);

    SDL_Rect dst = {0, 0, t->w, t->h};

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    /* Copy from the window, not from whatever target happens to be bound:
     * reading a texture means reading the texture, whatever the caller was
     * mid-way through doing. */
    SDL_SetRenderTarget(r, NULL);
    SDL_RenderSetClipRect(r, NULL);
    SDL_SetTextureBlendMode(t->sdl, SDL_BLENDMODE_NONE);
    SDL_RenderClear(r);
    SDL_RenderCopy(r, t->sdl, NULL, &dst);
    SDL_RenderReadPixels(r, &dst, SDL_PIXELFORMAT_ABGR8888, s->pixels, s->pitch);

    SDL_SetRenderTarget(r, saved_target);
    SDL_RenderSetClipRect(r, &saved_clip);
    SDL_SetRenderDrawBlendMode(r, saved_draw_blend);
    SDL_SetRenderDrawColor(r, cr, cg, cb, ca);
    SDL_SetTextureBlendMode(t->sdl, saved_tex_blend);
    return s;
}

AfColor af_texture_get_pixel(const AfTexture *t, int x, int y) {
    if (!t || x < 0 || y < 0 || x >= t->w || y >= t->h) return af_color_clear();
    SDL_Surface *s = texture_readback((AfTexture *)t);
    if (!s) return af_color_black();
    Uint32 px = ((Uint32 *)s->pixels)[y * (s->pitch / 4) + x];
    Uint8 r, g, b, a;
    SDL_GetRGBA(px, s->format, &r, &g, &b, &a);
    SDL_FreeSurface(s);
    return af_color(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

int af_texture_save_png(AfTexture *t, const char *path) {
    if (!t || !path) return 0;
    SDL_Surface *s = texture_readback(t);
    if (!s) {
        AF_WARN("could not read back texture for '%s'", path);
        return 0;
    }
    char resolved[1024];
    const char *data = af_platform_data_dir();
    if (data && *data && !af_fs_is_absolute(path)) {
        af_fs_path_join(data, path, resolved, (int)sizeof(resolved));
        path = resolved;
    }
    int ok = IMG_SavePNG(s, path) == 0;
    if (!ok) AF_WARN("could not write '%s': %s", path, IMG_GetError());
    SDL_FreeSurface(s);
    return ok;
}

void af_texture_cache_clear(void) {
    for (int i = 0; i < AF_TEX_HASH_SIZE; i++) {
        AfTexture *t = g_tex_cache.buckets[i];
        while (t) {
            AfTexture *next = t->next_hash;
            if (t->sdl) SDL_DestroyTexture(t->sdl);
            af_free(t);
            t = next;
        }
        g_tex_cache.buckets[i] = NULL;
    }
    g_tex_cache.count = 0;
    g_tex_white = NULL;
    g_tex_default = NULL;
}

int af_texture_cache_count(void) { return g_tex_cache.count; }

/* --------------------------------------------------- shared singletons */

AfTexture *af_texture_white(AfRenderer *r) {
    AF_UNUSED(r);
    if (g_tex_white) return g_tex_white;
    Uint32 px = 0xFFFFFFFFu;
    g_tex_white = af_texture_create(&px, 1, 1, 4, AF_FILTER_NEAREST);
    return g_tex_white;
}

AfTexture *af_texture_default_sprite(void) {
    if (g_tex_default) return g_tex_default;
    /* Opaque white. Anything drawn with the fallback still shows up, and a
     * tinted quad still tints, so a missing texture is a plain rectangle
     * rather than a hole in the frame. */
    static const Uint32 px[4] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
    g_tex_default = af_texture_create(px, 2, 2, 8, AF_FILTER_NEAREST);
    return g_tex_default;
}

/* --------------------------------------------------- procedural helpers */

AfTexture *af_texture_solid(int w, int h, AfColor c) {
    if (w <= 0 || h <= 0) return NULL;
    Uint32 *px = (Uint32 *)af_malloc((size_t)w * (size_t)h * 4);
    Uint32 v = pack(c);
    for (int i = 0; i < w * h; i++) px[i] = v;
    AfTexture *t = af_texture_create(px, w, h, w * 4, AF_FILTER_NEAREST);
    af_free(px);
    return t;
}

AfTexture *af_texture_checker(int cell, AfColor a, AfColor b) {
    if (cell <= 0) cell = 8;
    int n = cell * 2;
    Uint32 *px = (Uint32 *)af_malloc((size_t)n * (size_t)n * 4);
    Uint32 va = pack(a), vb = pack(b);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            int on = ((x / cell) + (y / cell)) & 1;
            px[y * n + x] = on ? vb : va;
        }
    AfTexture *t = af_texture_create(px, n, n, n * 4, AF_FILTER_NEAREST);
    af_free(px);
    return t;
}

AfTexture *af_texture_circle(int size, AfColor fill, AfColor edge) {
    if (size < 2) size = 2;
    Uint32 *px = (Uint32 *)af_calloc((size_t)size * (size_t)size, 4);
    float r = (float)size * 0.5f;
    Uint32 vf = pack(fill), ve = pack(edge);
    int has_edge = edge.a > 0.0f;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            float dx = (float)x + 0.5f - r, dy = (float)y + 0.5f - r;
            float d = sqrtf(dx * dx + dy * dy);
            if (d <= r - 1.0f) px[y * size + x] = vf;
            else if (has_edge && d <= r) px[y * size + x] = ve;
        }
    AfTexture *t = af_texture_create(px, size, size, size * 4, AF_FILTER_NEAREST);
    af_free(px);
    return t;
}

AfTexture *af_texture_rounded_rect(int w, int h, int radius, AfColor c) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (radius < 0) radius = 0;
    if (radius > af_mini(w, h) / 2) radius = af_mini(w, h) / 2;
    Uint32 *px = (Uint32 *)af_calloc((size_t)w * (size_t)h, 4);
    Uint32 v = pack(c);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int inside = 1;
            if (radius > 0) {
                int cx = -1, cy = -1;
                if (x < radius) cx = radius;
                else if (x >= w - radius) cx = w - radius - 1;
                if (y < radius) cy = radius;
                else if (y >= h - radius) cy = h - radius - 1;
                if (cx >= 0 && cy >= 0) {
                    float dx = (float)x - cx, dy = (float)y - cy;
                    if (dx * dx + dy * dy > (float)radius * (float)radius) inside = 0;
                }
            }
            if (inside) px[y * w + x] = v;
        }
    AfTexture *t = af_texture_create(px, w, h, w * 4, AF_FILTER_NEAREST);
    af_free(px);
    return t;
}

AfTexture *af_texture_gradient_v(int w, int h, AfColor top, AfColor bottom) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    Uint32 *px = (Uint32 *)af_malloc((size_t)w * (size_t)h * 4);
    for (int y = 0; y < h; y++) {
        AfColor c = af_color_mix(top, bottom, h > 1 ? (float)y / (float)(h - 1) : 0.0f);
        Uint32 v = pack(c);
        for (int x = 0; x < w; x++) px[y * w + x] = v;
    }
    AfTexture *t = af_texture_create(px, w, h, w * 4, AF_FILTER_NEAREST);
    af_free(px);
    return t;
}

AfTexture *af_texture_noise(int w, int h, AfColor a, AfColor b, uint32_t seed) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    Uint32 *px = (Uint32 *)af_malloc((size_t)w * (size_t)h * 4);
    AfRng rng = af_rng_seed(seed);
    for (int i = 0; i < w * h; i++)
        px[i] = pack(af_color_mix(a, b, af_rng_f32(&rng)));
    AfTexture *t = af_texture_create(px, w, h, w * 4, AF_FILTER_NEAREST);
    af_free(px);
    return t;
}

AfTexture *af_texture_outline(AfTexture *src, AfColor c, int margin) {
    if (!src || !src->sdl) return NULL;
    if (margin < 0) margin = 0;
    int w = src->w + margin * 2, h = src->h + margin * 2;
    if (w < 1 || h < 1) return NULL;
    Uint32 *px = (Uint32 *)af_calloc((size_t)w * (size_t)h, 4);
    Uint32 vc = pack(c);

    SDL_Surface *s = texture_readback(src);
    if (s) {
        memcpy(px + (size_t)margin * w + margin, s->pixels,
               (size_t)src->w * (size_t)src->h * 4);
        SDL_FreeSurface(s);
    } else {
        AF_WARN("outline needs an active renderer");
        af_free(px);
        return NULL;
    }

    /* Ring any transparent pixel that touches an opaque one. Writing into the
     * buffer as we go would grow the ring, so only fill cells that were
     * empty when the pass started: snapshot into a copy. */
    Uint32 *base = (Uint32 *)af_malloc((size_t)w * (size_t)h * 4);
    memcpy(base, px, (size_t)w * (size_t)h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            if (base[y * w + x] != 0) continue;
            int near = 0;
            for (int dy = -1; dy <= 1 && !near; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    if (base[ny * w + nx] != 0) { near = 1; break; }
                }
            if (near) px[y * w + x] = vc;
        }
    af_free(base);
    AfTexture *t = af_texture_create(px, w, h, w * 4, AF_FILTER_NEAREST);
    af_free(px);
    return t;
}

/* ================================================================= camera */

AfCamera2D af_camera_default(void) {
    AfCamera2D c;
    memset(&c, 0, sizeof(c));
    c.zoom = 1.0f;
    return c;
}

AfCamera2D af_camera_at(AfVec2 position, float zoom) {
    AfCamera2D c = af_camera_default();
    c.position = position;
    c.zoom = zoom > 0.0f ? zoom : 1.0f;
    return c;
}

AfMat3 af_camera_view_matrix(AfCamera2D cam) {
    AfVec2 focus = af_v2add(cam.position, cam.offset);
    float z = cam.zoom > 0.0001f ? cam.zoom : 1.0f;
    /* screen = T(view/2) * R * S(z) * T(-focus)
     * The half-view translation has to be its own step on the left: folding
     * it into the TRS would rotate the focus offset along with the camera. */
    AfMat3 to_origin =
        af_mat3_mul(af_mat3_translate(af_v2(cam.size.x * 0.5f, cam.size.y * 0.5f)),
                    af_mat3_trs(af_v2s(0.0f), cam.rotation, af_v2s(z)));
    return af_mat3_mul(to_origin, af_mat3_translate(af_v2neg(focus)));
}

AfVec2 af_camera_to_screen(AfCamera2D cam, AfVec2 world) {
    return af_mat3_xform(af_camera_view_matrix(cam), world);
}
AfVec2 af_camera_to_world(AfCamera2D cam, AfVec2 screen) {
    return af_mat3_xform(af_mat3_invert(af_camera_view_matrix(cam)), screen);
}

AfCamera2D af_camera_clamp(AfCamera2D cam, AfRect bounds) {
    if (af_rect_is_empty(bounds)) return cam;
    float z = cam.zoom > 0.0001f ? cam.zoom : 1.0f;
    float half_w = cam.size.x * 0.5f / z;
    float half_h = cam.size.y * 0.5f / z;
    float bw = bounds.size.x, bh = bounds.size.y;
    float x = cam.position.x, y = cam.position.y;
    if (bw <= half_w * 2.0f) x = af_rect_center_x(bounds);
    else x = af_clampf(x, bounds.pos.x + half_w, af_rect_right(bounds) - half_w);
    if (bh <= half_h * 2.0f) y = af_rect_center_y(bounds);
    else y = af_clampf(y, bounds.pos.y + half_h, af_rect_bottom(bounds) - half_h);
    cam.position = af_v2(x, y);
    return cam;
}

AfCamera2D af_camera_follow(AfCamera2D cam, AfVec2 target, float half_life,
                            float dt, AfRect bounds) {
    if (half_life <= 0.0f) cam.position = target;
    else cam.position = af_v2(af_damp(cam.position.x, target.x, half_life, dt),
                              af_damp(cam.position.y, target.y, half_life, dt));
    if (!af_rect_is_empty(bounds)) cam = af_camera_clamp(cam, bounds);
    return cam;
}

/* ============================================================== renderer */

typedef struct {
    AfTexture* tex;
    AfRect   dst;
    AfRect   uv;
    AfColor  tint;
    float    rotation;
    AfVec2   pivot;
    int      layer;
    float    sort_key;
    uint32_t tex_id;
} AfSpriteRec;

struct AfRenderer {
    SDL_Renderer* sdl;
    AfVec2   screen;
    float    scale;
    AfCamera2D cam;
    AfMat3   view;
    int      view_dirty;
    /* clip */
    AfRect   clip_stack[AF_MAX_CLIP_DEPTH];
    int      clip_depth;
    AfRect   applied_clip;
    /* batch */
    SDL_Texture* batch_tex;
    SDL_Vertex*  verts;
    int      vert_count, vert_cap;
    /* sprites */
    AfSpriteRec* sprites;
    int      sprite_count, sprite_cap;
    int      flushing;
    /* stats */
    AfRenderStats stats;
    AfStopwatch   sw;
    struct AfFontSize* last_atlas;
};

static AfRenderer *g_current;

AfRenderer *af_r2d_current(void) { return g_current; }

void af_r2d_set_texture_context(AfRenderer *r) { g_tex_ctx = r; }
AfRenderer *af_r2d_texture_context(void) { return g_tex_ctx; }
SDL_Renderer *af_r2d_sdl(AfRenderer *r) { return r ? r->sdl : NULL; }

/* Convenience so `af_r2d_of_window(win)` reads the same as the SDL-ish
 * alternative. The window owns the renderer, so this is just a fetch. */
AfRenderer *af_r2d_of_window(void *window) {
    return window ? af_window_renderer((AfWindow *)window) : NULL;
}

AfRenderer *af_r2d_create(SDL_Renderer *sdl, int width, int height, float scale) {
    AfRenderer *r = (AfRenderer *)af_calloc(1, sizeof(AfRenderer));
    r->sdl = sdl;
    r->screen = af_v2((float)width, (float)height);
    r->scale = scale;
    r->cam = af_camera_default();
    r->cam.size = r->screen;
    r->view = af_camera_view_matrix(r->cam);
    r->view_dirty = 0;
    r->clip_stack[0] = af_rect(0, 0, (float)width, (float)height);
    r->applied_clip = r->clip_stack[0];
    g_tex_ctx = r;
    return r;
}

void af_r2d_destroy(AfRenderer *r) {
    if (!r) return;
    af_free(r->verts);
    af_free(r->sprites);
    af_free(r);
}

void af_r2d_set_size(AfRenderer *r, int w, int h, float scale) {
    if (!r) return;
    r->screen = af_v2((float)w, (float)h);
    r->scale = scale;
    r->cam.size = r->screen;
    r->clip_stack[0] = af_rect(0, 0, (float)w, (float)h);
    r->clip_depth = 0;
    /* Force the new root clip to be re-applied on the next submit. */
    r->applied_clip.pos.x = -1.0f;
    r->applied_clip.pos.y = -1.0f;
    r->applied_clip.size.x = -1.0f;
    r->applied_clip.size.y = -1.0f;
    r->view_dirty = 1;
}

/* -------------------------------------------------------- batch internals */

static void batch_grow(AfRenderer *r, int need) {
    if (r->vert_count + need <= r->vert_cap) return;
    int cap = r->vert_cap ? r->vert_cap : AF_VERT_INITIAL;
    while (cap < r->vert_count + need) cap *= 2;
    r->verts = (SDL_Vertex *)af_realloc(r->verts, sizeof(SDL_Vertex) * (size_t)cap);
    r->vert_cap = cap;
}

static void apply_clip(AfRenderer *r, AfRect clip) {
    if (af_rect_eq(r->applied_clip, clip)) return;
    r->applied_clip = clip;
    if (!r->sdl) return;
    if (af_rect_is_empty(clip)) SDL_RenderSetClipRect(r->sdl, NULL);
    else {
        SDL_Rect rc;
        rc.x = (int)floorf(clip.pos.x);
        rc.y = (int)floorf(clip.pos.y);
        rc.w = (int)ceilf(clip.size.x);
        rc.h = (int)ceilf(clip.size.y);
        SDL_RenderSetClipRect(r->sdl, &rc);
    }
}

static void batch_submit(AfRenderer *r) {
    if (r->vert_count == 0) return;
    apply_clip(r, r->clip_stack[r->clip_depth]);
    if (r->sdl) {
        /* A NULL texture is the untextured path: vertex colors only. */
        SDL_RenderGeometry(r->sdl, r->batch_tex, r->verts, r->vert_count, NULL, 0);
        r->stats.draw_calls++;
        r->stats.batches++;
    }
    r->stats.vertices += r->vert_count;
    r->vert_count = 0;
}

static void batch_break(AfRenderer *r) {
    batch_submit(r);
    r->batch_tex = NULL;
}

static void set_vert(SDL_Vertex *v, float x, float y, float u, float vv,
                     Uint32 color) {
    v->position.x = x;
    v->position.y = y;
    v->tex_coord.x = u;
    v->tex_coord.y = vv;
    v->color.r = (Uint8)((color >> 16) & 0xFF);
    v->color.g = (Uint8)((color >> 8) & 0xFF);
    v->color.b = (Uint8)(color & 0xFF);
    v->color.a = (Uint8)((color >> 24) & 0xFF);
}

static void ensure_view(AfRenderer *r) {
    if (!r->view_dirty) return;
    r->view = af_camera_view_matrix(r->cam);
    r->view_dirty = 0;
}

static void start_batch(AfRenderer *r, AfTexture *tex) {
    if (r->batch_tex != (tex ? tex->sdl : NULL)) {
        batch_break(r);
        r->batch_tex = tex ? tex->sdl : NULL;
    }
}

/* One transformed, textured quad in world space. */
static void push_quad(AfRenderer *r, AfTexture *tex, AfVec2 center, AfVec2 half,
                      float rotation, AfRect uv, AfColor tint) {
    if (af_absf(tint.a) < 0.002f) return;
    ensure_view(r);
    start_batch(r, tex);

    int tw = tex ? af_maxi(tex->w, 1) : 1;
    int th = tex ? af_maxi(tex->h, 1) : 1;
    float u0 = uv.pos.x / (float)tw, v0 = uv.pos.y / (float)th;
    float u1 = af_rect_right(uv) / (float)tw, v1 = af_rect_bottom(uv) / (float)th;
    Uint32 col = pack(tint);

    const float xs[4] = {-half.x, half.x, half.x, -half.x};
    const float ys[4] = {-half.y, -half.y, half.y, half.y};
    const float us[4] = {u0, u1, u1, u0};
    const float vs[4] = {v0, v0, v1, v1};

    batch_grow(r, 6);
    SDL_Vertex *v = r->verts + r->vert_count;
    float c = 1.0f, s = 0.0f;
    if (af_absf(rotation) > 0.0001f) { c = cosf(rotation); s = sinf(rotation); }
    for (int i = 0; i < 4; i++) {
        float wx = center.x + xs[i] * c - ys[i] * s;
        float wy = center.y + xs[i] * s + ys[i] * c;
        float sx = r->view.m[0] * wx + r->view.m[1] * wy + r->view.m[2];
        float sy = r->view.m[3] * wx + r->view.m[4] * wy + r->view.m[5];
        set_vert(&v[i], sx, sy, us[i], vs[i], col);
    }
    set_vert(&v[4], v[3].position.x, v[3].position.y, v[3].tex_coord.x,
             v[3].tex_coord.y, col);
    set_vert(&v[5], v[0].position.x, v[0].position.y, v[0].tex_coord.x,
             v[0].tex_coord.y, col);
    r->vert_count += 6;
    r->stats.quads++;
    r->stats.triangles += 2;
}

static void push_tri(AfRenderer *r, AfVec2 a, AfVec2 b, AfVec2 c, AfColor col) {
    if (af_absf(col.a) < 0.002f) return;
    ensure_view(r);
    /* Shapes are untextured, so they cannot share a batch with a sprite. */
    start_batch(r, NULL);
    batch_grow(r, 3);
    SDL_Vertex *v = r->verts + r->vert_count;
    Uint32 p = pack(col);
    const AfVec2 src[3] = {a, b, c};
    for (int i = 0; i < 3; i++) {
        float sx = r->view.m[0] * src[i].x + r->view.m[1] * src[i].y + r->view.m[2];
        float sy = r->view.m[3] * src[i].x + r->view.m[4] * src[i].y + r->view.m[5];
        set_vert(&v[i], sx, sy, 0.5f, 0.5f, p);
    }
    r->vert_count += 3;
    r->stats.triangles++;
}

/* ============================================================ frame cycle */

void af_r2d_begin(AfRenderer *r, AfColor clear_color) {
    if (!r) return;
    g_current = r;
    g_tex_ctx = r;
    af_stopwatch_start(&r->sw);
    if (r->sdl) {
        SDL_SetRenderDrawBlendMode(r->sdl, SDL_BLENDMODE_BLEND);
        apply_clip(r, r->clip_stack[0]);
    }
    af_r2d_clear(r, clear_color);
    r->clip_depth = 0;
    r->batch_tex = NULL;
    r->vert_count = 0;
    r->sprite_count = 0;
    memset(&r->stats, 0, sizeof(r->stats));
}

void af_r2d_end(AfRenderer *r) {
    if (!r) return;
    af_r2d_flush_sprites(r);
    batch_break(r);
    if (r->sdl) {
        SDL_RenderSetClipRect(r->sdl, NULL);
        SDL_RenderPresent(r->sdl);
    }
    r->stats.cpu_ms = (float)af_stopwatch_elapsed(&r->sw);
    if (g_current == r) g_current = NULL;
}

void af_r2d_clear(AfRenderer *r, AfColor c) {
    if (!r || !r->sdl) return;
    batch_break(r);
    SDL_SetRenderDrawColor(r->sdl, (Uint8)(af_clamp01(c.r) * 255.0f),
                           (Uint8)(af_clamp01(c.g) * 255.0f),
                           (Uint8)(af_clamp01(c.b) * 255.0f),
                           (Uint8)(af_clamp01(c.a) * 255.0f));
    SDL_RenderClear(r->sdl);
}

AfVec2 af_r2d_screen_size(AfRenderer *r) { return r ? r->screen : af_v2s(0); }
float af_r2d_render_scale(AfRenderer *r) { return r ? r->scale : 1.0f; }

int af_r2d_read_pixels(AfRenderer *r, void *dst, int w, int h) {
    if (!r || !r->sdl || !dst || w <= 0 || h <= 0) return 0;
    /* Flush first: capturing a frame that is still sitting in a batch would
     * otherwise miss everything drawn since the last batch break. */
    af_r2d_flush_sprites(r);
    batch_break(r);
    SDL_Rect rect = {0, 0, w, h};
    /* ABGR8888, matching the texture layout the rest of the renderer uses. */
    return SDL_RenderReadPixels(r->sdl, &rect, SDL_PIXELFORMAT_ABGR8888, dst,
                                w * 4) == 0;
}

void af_r2d_set_camera(AfRenderer *r, AfCamera2D cam) {
    if (!r) return;
    cam.size = r->screen;
    r->cam = cam;
    r->view_dirty = 1;
}

AfCamera2D af_r2d_get_camera(AfRenderer *r) {
    return r ? r->cam : af_camera_default();
}

AfVec2 af_r2d_screen_to_world(AfRenderer *r, AfVec2 p) {
    return r ? af_camera_to_world(r->cam, p) : p;
}
AfVec2 af_r2d_world_to_screen(AfRenderer *r, AfVec2 p) {
    return r ? af_camera_to_screen(r->cam, p) : p;
}
AfRect af_r2d_screen_rect_to_world(AfRenderer *r, AfRect s) {
    AfVec2 a = af_r2d_screen_to_world(r, s.pos);
    AfVec2 b = af_r2d_screen_to_world(r, af_v2add(s.pos, s.size));
    return af_rect_from_minmax(af_minf(a.x, b.x), af_minf(a.y, b.y),
                               af_maxf(a.x, b.x), af_maxf(a.y, b.y));
}
AfRect af_r2d_visible_world_rect(AfRenderer *r) {
    if (!r) return af_rect(0, 0, 0, 0);
    return af_r2d_screen_rect_to_world(r, af_rect(0, 0, r->screen.x, r->screen.y));
}

void af_r2d_push_clip(AfRenderer *r, AfRect rect) {
    if (!r) return;
    if (r->clip_depth + 1 >= AF_MAX_CLIP_DEPTH) {
        AF_WARN("clip stack overflow (max %d)", AF_MAX_CLIP_DEPTH);
        return;
    }
    /* Nested clips intersect, so a child cannot draw outside its parent. */
    AfRect parent = r->clip_stack[r->clip_depth];
    batch_break(r);
    r->clip_depth++;
    r->clip_stack[r->clip_depth] = af_rect_intersect(rect, parent);
}

void af_r2d_pop_clip(AfRenderer *r) {
    if (!r || r->clip_depth <= 0) return;
    batch_break(r);
    r->clip_depth--;
}

AfRect af_r2d_clip(AfRenderer *r) {
    return r ? r->clip_stack[r->clip_depth] : af_rect(0, 0, 0, 0);
}

/* ================================================================ sprites */

void af_r2d_sprite(AfRenderer *r, AfTexture *tex, AfRect dst, AfRect uv,
                   AfColor tint, float rotation, AfVec2 pivot, int layer,
                   float sort_key) {
    if (!r) return;
    if (!tex || !tex->sdl) tex = af_texture_default_sprite();
    if (uv.size.x <= 0.0f && uv.size.y <= 0.0f)
        uv = af_rect(0, 0, (float)tex->w, (float)tex->h);

    if (r->sprite_count >= AF_SPRITE_MAX) {
        /* Overflow: emit what we have so the frame still looks complete. */
        r->flushing++;
        af_r2d_flush_sprites(r);
        r->flushing--;
    }
    if (r->sprite_count == r->sprite_cap) {
        int cap = r->sprite_cap ? r->sprite_cap * 2 : 2048;
        r->sprites = (AfSpriteRec *)af_realloc(r->sprites,
                                              sizeof(AfSpriteRec) * (size_t)cap);
        r->sprite_cap = cap;
    }
    AfSpriteRec *s = &r->sprites[r->sprite_count++];
    s->tex = tex;
    s->dst = dst;
    s->uv = uv;
    s->tint = tint;
    s->rotation = rotation;
    s->pivot = pivot;
    s->layer = layer;
    /* sort_key is a nudge: 0 means "sort by my own Y", which is what a user
     * expects for a side-scroller without any extra work. */
    s->sort_key = af_rect_center_y(dst) + sort_key;
    s->tex_id = (uint32_t)(uintptr_t)tex->sdl;
}

void af_r2d_sprite_layer(AfRenderer *r, AfTexture *tex, AfRect dst, AfRect uv,
                         AfColor tint, int layer, float sort_key) {
    af_r2d_sprite(r, tex, dst, uv, tint, 0.0f, af_v2s(0.0f), layer, sort_key);
}

void af_r2d_sprite_simple(AfRenderer *r, AfTexture *tex, AfRect dst, AfColor tint,
                          int layer) {
    if (!tex) tex = af_texture_default_sprite();
    AfRect uv = af_rect(0, 0, (float)tex->w, (float)tex->h);
    af_r2d_sprite(r, tex, dst, uv, tint, 0.0f, af_v2s(0), layer, 0.0f);
}

void af_r2d_sprite_centered(AfRenderer *r, AfTexture *tex, AfVec2 center,
                            float scale, AfColor tint, float rotation, int layer) {
    if (!tex) tex = af_texture_default_sprite();
    AfVec2 size = af_v2((float)tex->w * scale, (float)tex->h * scale);
    af_r2d_sprite(r, tex, af_rect_centered(center, size),
                  af_rect(0, 0, (float)tex->w, (float)tex->h), tint, rotation,
                  af_v2s(0), layer, 0.0f);
}

void af_r2d_sprite_flipped(AfRenderer *r, int flip_x, int flip_y) {
    if (!r || r->sprite_count == 0) return;
    AfSpriteRec *s = &r->sprites[r->sprite_count - 1];
    if (flip_x) {
        float t = s->uv.pos.x;
        s->uv.pos.x = af_rect_right(s->uv);
        s->uv.size.x = -s->uv.size.x;
        AF_UNUSED(t);
    }
    if (flip_y) {
        s->uv.pos.y = af_rect_bottom(s->uv);
        s->uv.size.y = -s->uv.size.y;
    }
}

static int sprite_cmp(const void *a, const void *b) {
    const AfSpriteRec *x = (const AfSpriteRec *)a, *y = (const AfSpriteRec *)b;
    if (x->layer != y->layer) return x->layer < y->layer ? -1 : 1;
    if (x->sort_key != y->sort_key) return x->sort_key < y->sort_key ? -1 : 1;
    if (x->tex_id != y->tex_id) return x->tex_id < y->tex_id ? -1 : 1;
    return 0;
}

void af_r2d_flush_sprites(AfRenderer *r) {
    if (!r || r->sprite_count == 0 || r->flushing) return;
    r->flushing++;
    qsort(r->sprites, (size_t)r->sprite_count, sizeof(AfSpriteRec), sprite_cmp);
    for (int i = 0; i < r->sprite_count; i++) {
        AfSpriteRec *s = &r->sprites[i];
        AfVec2 half = af_v2(s->dst.size.x * 0.5f, s->dst.size.y * 0.5f);
        /* pivot is normalised 0..1; shift the centre to compensate. */
        AfVec2 center = af_v2add(af_rect_center(s->dst),
                                 af_v2(half.x * (s->pivot.x * 2.0f - 1.0f),
                                       half.y * (s->pivot.y * 2.0f - 1.0f)));
        /* The sort already happened in world order, so draw through the
         * batcher with the camera as-is. */
        push_quad(r, s->tex, center, half, s->rotation, s->uv, s->tint);
    }
    r->stats.sprite_count += r->sprite_count;
    r->sprite_count = 0;
    batch_break(r);
    r->flushing--;
}

void af_r2d_clear_sprites(AfRenderer *r) {
    if (r) r->sprite_count = 0;
}
int af_r2d_sprite_count(AfRenderer *r) { return r ? r->sprite_count : 0; }

/* ============================================================ direct draws */

void af_r2d_quad(AfRenderer *r, AfTexture *tex, AfVec2 center, AfVec2 half,
                 float rotation, AfRect uv, AfColor tint) {
    if (!r) return;
    if (!tex) tex = af_texture_white(r);
    push_quad(r, tex, center, half, rotation, uv, tint);
}

void af_r2d_tex_rect(AfRenderer *r, AfTexture *tex, AfRect dst, AfRect uv,
                     AfColor tint) {
    if (!r) return;
    if (!tex || !tex->sdl) { af_r2d_fill(r, dst, tint); return; }
    push_quad(r, tex, af_rect_center(dst),
              af_v2(dst.size.x * 0.5f, dst.size.y * 0.5f), 0.0f, uv, tint);
}

void af_r2d_fill(AfRenderer *r, AfRect rect, AfColor c) {
    if (!r) return;
    push_quad(r, af_texture_white(r), af_rect_center(rect),
              af_v2(rect.size.x * 0.5f, rect.size.y * 0.5f), 0.0f,
              af_rect(0, 0, 1, 1), c);
}

void af_r2d_screen_space_begin(AfRenderer *r, AfCamera2D *out_saved) {
    if (!r) return;
    if (out_saved) *out_saved = r->cam;
    /* A world camera puts whatever it is focused on at the centre of the view,
     * so focusing the default camera on the world origin would shift every
     * screen-space coordinate by half the screen. Focusing on the middle of
     * the view instead makes the view matrix the identity, which is what
     * "screen space" has to mean for (0,0) to be the top-left pixel. */
    r->cam = af_camera_default();
    r->cam.position = af_v2(r->screen.x * 0.5f, r->screen.y * 0.5f);
    r->cam.size = r->screen;
    r->view_dirty = 1;
}

void af_r2d_screen_space_end(AfRenderer *r, const AfCamera2D *saved) {
    if (!r || !saved) return;
    r->cam = *saved;
    r->view_dirty = 1;
}

void af_r2d_ui_fill(AfRenderer *r, AfRect rect, AfColor c) {
    if (!r) return;
    AfCamera2D saved;
    af_r2d_screen_space_begin(r, &saved);
    af_r2d_fill(r, rect, c);
    af_r2d_screen_space_end(r, &saved);
}

void af_r2d_ui_tex(AfRenderer *r, AfTexture *tex, AfRect dst, AfRect uv,
                   AfColor tint) {
    if (!r) return;
    AfCamera2D saved;
    af_r2d_screen_space_begin(r, &saved);
    af_r2d_tex_rect(r, tex, dst, uv, tint);
    af_r2d_screen_space_end(r, &saved);
}

void af_r2d_tri(AfRenderer *r, AfVec2 a, AfVec2 b, AfVec2 c, AfColor col) {
    if (!r) return;
    push_tri(r, a, b, c, col);
}

void af_r2d_line(AfRenderer *r, AfVec2 a, AfVec2 b, float thickness, AfColor c) {
    if (!r) return;
    float t = af_maxf(thickness, 0.35f);
    AfVec2 d = af_v2sub(b, a);
    if (af_v2len_sq(d) < 1e-8f) return;
    AfVec2 n = af_v2scale(af_v2perp(af_v2norm(d)), t * 0.5f);
    push_tri(r, af_v2add(a, n), af_v2sub(a, n), af_v2sub(b, n), c);
    push_tri(r, af_v2add(a, n), af_v2sub(b, n), af_v2add(b, n), c);
}

void af_r2d_line_aa(AfRenderer *r, AfVec2 a, AfVec2 b, float thickness,
                    AfColor c) {
    af_r2d_line(r, a, b, thickness, c);
}

void af_r2d_rect_outline(AfRenderer *r, AfRect rect, float thickness, AfColor c) {
    if (!r) return;
    AfVec2 p[4] = {rect.pos, af_v2(af_rect_right(rect), rect.pos.y),
                   af_rect_max(rect), af_v2(rect.pos.x, af_rect_bottom(rect))};
    for (int i = 0; i < 4; i++)
        af_r2d_line(r, p[i], p[(i + 1) & 3], thickness, c);
}

void af_r2d_ui_rect_outline(AfRenderer *r, AfRect rect, float thickness,
                            AfColor c) {
    if (!r) return;
    AfCamera2D saved;
    af_r2d_screen_space_begin(r, &saved);
    af_r2d_rect_outline(r, rect, thickness, c);
    af_r2d_screen_space_end(r, &saved);
}

void af_r2d_circle(AfRenderer *r, AfVec2 center, float radius, AfColor c,
                   int segments) {
    if (!r) return;
    if (segments < 3) segments = 3;
    if (segments > 256) segments = 256;
    for (int i = 0; i < segments; i++) {
        float a0 = (float)i / (float)segments * AF_TAU;
        float a1 = (float)(i + 1) / (float)segments * AF_TAU;
        push_tri(r, center, af_v2add(center, af_v2scale(af_v2from_angle(a0), radius)),
                 af_v2add(center, af_v2scale(af_v2from_angle(a1), radius)), c);
    }
}

void af_r2d_circle_outline(AfRenderer *r, AfVec2 center, float radius,
                           float thickness, AfColor c, int segments) {
    if (!r) return;
    if (segments < 3) segments = 3;
    if (segments > 256) segments = 256;
    AfVec2 prev = af_v2add(center, af_v2(radius, 0.0f));
    for (int i = 1; i <= segments; i++) {
        float a = (float)i / (float)segments * AF_TAU;
        AfVec2 cur = af_v2add(center, af_v2scale(af_v2from_angle(a), radius));
        af_r2d_line(r, prev, cur, thickness, c);
        prev = cur;
    }
}

void af_r2d_poly(AfRenderer *r, AfVec2 *pts, int count, AfColor c) {
    if (!r || count < 3) return;
    /* Fan from the centroid: works for convex and mildly concave shapes
     * without needing a triangulation pass. */
    AfVec2 center = {0, 0};
    for (int i = 0; i < count; i++) center = af_v2add(center, pts[i]);
    center = af_v2scale(center, 1.0f / (float)count);
    for (int i = 0; i < count; i++)
        push_tri(r, center, pts[i], pts[(i + 1) % count], c);
}

void af_r2d_poly_outline(AfRenderer *r, AfVec2 *pts, int count, float thickness,
                         AfColor c) {
    if (!r || count < 2) return;
    for (int i = 0; i < count; i++)
        af_r2d_line(r, pts[i], pts[(i + 1) % count], thickness, c);
}

void af_r2d_aabb(AfRenderer *r, AfBounds b, AfColor c) {
    if (!r) return;
    af_r2d_rect_outline(r, af_bounds_rect(b), 1.0f, c);
}

AfRenderStats af_r2d_stats(AfRenderer *r) {
    return r ? r->stats : (AfRenderStats){0};
}
void af_r2d_reset_stats(AfRenderer *r) {
    if (r) memset(&r->stats, 0, sizeof(r->stats));
}

void af_r2d_flush(AfRenderer *r) {
    if (r) batch_break(r);
}

void af_r2d_push_glyph_quad(AfRenderer *r, AfTexture *tex, AfVec2 pos,
                            AfVec2 size, AfRect uv, AfColor tint) {
    if (!r) return;
    push_quad(r, tex, af_v2add(pos, af_v2(size.x * 0.5f, size.y * 0.5f)),
              af_v2(size.x * 0.5f, size.y * 0.5f), 0.0f, uv, tint);
}
