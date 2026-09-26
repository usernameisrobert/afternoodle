/* Small, fast math: vec2/vec4/rect/mat3, angles, easing, deterministic RNG.
 *
 * Convention: right-handed, +Y down (screen space), angles in radians unless a
 * name ends in `_deg`. All functions are branch-light and header-inline so the
 * engine can run without linking libm-heavy helpers in hot paths.
 */
#ifndef AFNDLE_CORE_MATH_H
#define AFNDLE_CORE_MATH_H

#include <math.h>
#include <stdint.h>
#include <string.h>
#include "afndle/core/afconfig.h"

#define AF_PI      3.14159265358979323846f
#define AF_TAU     (AF_PI * 2.0f)
#define AF_HALF_PI (AF_PI * 0.5f)
#define AF_DEG2RAD (AF_PI / 180.0f)
#define AF_RAD2DEG (180.0f / AF_PI)
#define AF_EPS     1e-6f
#define AF_FLT_MAX 3.402823466e+38f

/* ------------------------------------------------------------------ scalar */
AF_INLINE float af_minf(float a, float b) { return a < b ? a : b; }
AF_INLINE float af_maxf(float a, float b) { return a > b ? a : b; }
AF_INLINE float af_clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
AF_INLINE int af_clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
AF_INLINE float af_clamp01(float v) { return af_clampf(v, 0.0f, 1.0f); }
AF_INLINE float af_absf(float v) { return v < 0.0f ? -v : v; }
AF_INLINE float af_signf(float v) { return v < 0.0f ? -1.0f : 1.0f; }
AF_INLINE float af_lerpf(float a, float b, float t) { return a + (b - a) * t; }
AF_INLINE float af_inv_lerpf(float a, float b, float v) {
    return fabsf(b - a) < AF_EPS ? 0.0f : (v - a) / (b - a);
}
AF_INLINE int af_mini(int a, int b) { return a < b ? a : b; }
AF_INLINE int af_maxi(int a, int b) { return a > b ? a : b; }
AF_INLINE int af_isfinited(float v) { return isfinite(v) != 0; }
AF_INLINE int af_signi(int v) { return v < 0 ? -1 : (v > 0 ? 1 : 0); }

/* Wrap v into [lo, hi). */
AF_INLINE float af_wrapf(float v, float lo, float hi) {
    float range = hi - lo;
    if (range <= AF_EPS) return lo;
    float r = fmodf(v - lo, range);
    if (r < 0.0f) r += range;
    return lo + r;
}
AF_INLINE float af_lerp_angle(float a, float b, float t) {
    return a + af_wrapf(b - a, -AF_PI, AF_PI) * t;
}
AF_INLINE float af_move_toward(float cur, float target, float max_delta) {
    float d = target - cur;
    if (af_absf(d) <= max_delta) return target;
    return cur + af_signf(d) * max_delta;
}
AF_INLINE float af_approach(float cur, float target, float rate, float dt) {
    return af_lerpf(cur, target, af_clamp01(rate * dt));
}
AF_INLINE float af_smoothstep(float t) {
    t = af_clamp01(t);
    return t * t * (3.0f - 2.0f * t);
}
AF_INLINE float af_smootherstep(float t) {
    t = af_clamp01(t);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}
/* Framerate-independent exponential smoothing. */
AF_INLINE float af_damp(float cur, float target, float half_life, float dt) {
    if (half_life <= AF_EPS) return target;
    return target + (cur - target) * exp2f(-dt / half_life);
}
AF_INLINE float af_pingpong(float t) {
    t = af_wrapf(t, 0.0f, 2.0f);
    return t < 1.0f ? t : 2.0f - t;
}

/* ------------------------------------------------------------------- vec2 */
typedef struct { float x, y; } AfVec2;

AF_INLINE AfVec2 af_v2(float x, float y) { AfVec2 v = {x, y}; return v; }
AF_INLINE AfVec2 af_v2s(float s) { return af_v2(s, s); }
AF_INLINE AfVec2 af_v2add(AfVec2 a, AfVec2 b) { return af_v2(a.x + b.x, a.y + b.y); }
AF_INLINE AfVec2 af_v2sub(AfVec2 a, AfVec2 b) { return af_v2(a.x - b.x, a.y - b.y); }
AF_INLINE AfVec2 af_v2mul(AfVec2 a, AfVec2 b) { return af_v2(a.x * b.x, a.y * b.y); }
AF_INLINE AfVec2 af_v2div(AfVec2 a, AfVec2 b) { return af_v2(a.x / b.x, a.y / b.y); }
AF_INLINE AfVec2 af_v2scale(AfVec2 a, float s) { return af_v2(a.x * s, a.y * s); }
AF_INLINE AfVec2 af_v2neg(AfVec2 a) { return af_v2(-a.x, -a.y); }
AF_INLINE float  af_v2dot(AfVec2 a, AfVec2 b) { return a.x * b.x + a.y * b.y; }
AF_INLINE float  af_v2cross(AfVec2 a, AfVec2 b) { return a.x * b.y - a.y * b.x; }
AF_INLINE float  af_v2len(AfVec2 a) { return sqrtf(af_v2dot(a, a)); }
AF_INLINE float  af_v2len_sq(AfVec2 a) { return af_v2dot(a, a); }
AF_INLINE AfVec2 af_v2norm(AfVec2 a) {
    float l = af_v2len(a);
    return l > AF_EPS ? af_v2scale(a, 1.0f / l) : af_v2(0.0f, 0.0f);
}
AF_INLINE AfVec2 af_v2perp(AfVec2 a) { return af_v2(-a.y, a.x); } /* rotate +90 */
AF_INLINE AfVec2 af_v2rot(AfVec2 a, float rad) {
    float c = cosf(rad), s = sinf(rad);
    return af_v2(a.x * c - a.y * s, a.x * s + a.y * c);
}
AF_INLINE AfVec2 af_v2rot_around(AfVec2 a, AfVec2 pivot, float rad) {
    return af_v2add(pivot, af_v2rot(af_v2sub(a, pivot), rad));
}
AF_INLINE AfVec2 af_v2clamp(AfVec2 a, float lo, float hi) {
    return af_v2(af_clampf(a.x, lo, hi), af_clampf(a.y, lo, hi));
}
AF_INLINE AfVec2 af_v2lerp(AfVec2 a, AfVec2 b, float t) {
    return af_v2(af_lerpf(a.x, b.x, t), af_lerpf(a.y, b.y, t));
}
AF_INLINE AfVec2 af_v2min(AfVec2 a, AfVec2 b) {
    return af_v2(af_minf(a.x, b.x), af_minf(a.y, b.y));
}
AF_INLINE AfVec2 af_v2max(AfVec2 a, AfVec2 b) {
    return af_v2(af_maxf(a.x, b.x), af_maxf(a.y, b.y));
}
AF_INLINE AfVec2 af_v2abs(AfVec2 a) { return af_v2(af_absf(a.x), af_absf(a.y)); }
AF_INLINE int    af_v2eq(AfVec2 a, AfVec2 b) {
    return fabsf(a.x - b.x) < AF_EPS && fabsf(a.y - b.y) < AF_EPS;
}
AF_INLINE AfVec2 af_v2from_angle(float rad) { return af_v2(cosf(rad), sinf(rad)); }
AF_INLINE AfVec2 af_v2from_angle_deg(float deg) {
    return af_v2from_angle(deg * AF_DEG2RAD);
}
AF_INLINE float  af_v2angle(AfVec2 a) { return atan2f(a.y, a.x); }
AF_INLINE AfVec2 af_v2snap(AfVec2 a, float grid) {
    return af_v2(roundf(a.x / grid) * grid, roundf(a.y / grid) * grid);
}

/* ------------------------------------------------------------------- vec3 */
typedef struct { float x, y, z; } AfVec3;
AF_INLINE AfVec3 af_v3(float x, float y, float z) { AfVec3 v = {x, y, z}; return v; }
AF_INLINE AfVec3 af_v3add(AfVec3 a, AfVec3 b) { return af_v3(a.x + b.x, a.y + b.y, a.z + b.z); }
AF_INLINE AfVec3 af_v3sub(AfVec3 a, AfVec3 b) { return af_v3(a.x - b.x, a.y - b.y, a.z - b.z); }
AF_INLINE AfVec3 af_v3scale(AfVec3 a, float s) { return af_v3(a.x * s, a.y * s, a.z * s); }
AF_INLINE float  af_v3dot(AfVec3 a, AfVec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
AF_INLINE float  af_v3len(AfVec3 a) { return sqrtf(af_v3dot(a, a)); }

/* ------------------------------------------------------------------- vec4 */
/* xy = position, z = width, w = height  (also used as RGBA colour). */
typedef struct { float x, y, z, w; } AfVec4;

AF_INLINE AfVec4 af_v4(float x, float y, float z, float w) {
    AfVec4 v = {x, y, z, w};
    return v;
}
AF_INLINE AfVec4 af_v4s(float s) { return af_v4(s, s, s, s); }
AF_INLINE AfVec4 af_v4add(AfVec4 a, AfVec4 b) {
    return af_v4(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w);
}
AF_INLINE AfVec4 af_v4sub(AfVec4 a, AfVec4 b) {
    return af_v4(a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w);
}
AF_INLINE AfVec4 af_v4scale(AfVec4 a, float s) {
    return af_v4(a.x * s, a.y * s, a.z * s, a.w * s);
}
AF_INLINE AfVec4 af_v4lerp(AfVec4 a, AfVec4 b, float t) {
    return af_v4(af_lerpf(a.x, b.x, t), af_lerpf(a.y, b.y, t),
                 af_lerpf(a.z, b.z, t), af_lerpf(a.w, b.w, t));
}
AF_INLINE AfVec2 af_v4xy(AfVec4 v) { return af_v2(v.x, v.y); }
AF_INLINE AfVec2 af_v4zw(AfVec4 v) { return af_v2(v.z, v.w); }

/* ------------------------------------------------------------------ colour */
/* Stored as 0..1 linear-ish floats. Renderer applies sRGB on upload. */
typedef struct { float r, g, b, a; } AfColor;

AF_INLINE AfColor af_color(float r, float g, float b, float a) {
    AfColor c = {r, g, b, a};
    return c;
}
AF_INLINE AfColor af_rgb8(int r, int g, int b) {
    return af_color((float)r / 255.0f, (float)g / 255.0f, (float)b / 255.0f, 1.0f);
}
AF_INLINE AfColor af_rgba8(int r, int g, int b, int a) {
    return af_color((float)r / 255.0f, (float)g / 255.0f, (float)b / 255.0f,
                    (float)a / 255.0f);
}
/** 0xRRGGBBAA */
AF_INLINE AfColor af_color_hex(uint32_t hex) {
    return af_color(((hex >> 24) & 0xFF) / 255.0f, ((hex >> 16) & 0xFF) / 255.0f,
                    ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f);
}
AF_INLINE AfColor af_color_with_a(AfColor c, float a) { c.a = a; return c; }
AF_INLINE AfColor af_color_mix(AfColor a, AfColor b, float t) {
    return af_color(af_lerpf(a.r, b.r, t), af_lerpf(a.g, b.g, t),
                    af_lerpf(a.b, b.b, t), af_lerpf(a.a, b.a, t));
}
AF_INLINE AfColor af_color_scale(AfColor c, float s) {
    return af_color(c.r * s, c.g * s, c.b * s, c.a);
}
/** Exact channel compare; for tests and cache keys, not per-frame drawing. */
AF_INLINE int af_color_eq(AfColor a, AfColor b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}
AF_INLINE AfColor af_color_white(void) { return af_color(1, 1, 1, 1); }
AF_INLINE AfColor af_color_black(void) { return af_color(0, 0, 0, 1); }
AF_INLINE AfColor af_color_clear(void) { return af_color(0, 0, 0, 0); }
AF_INLINE uint32_t af_color_to_rgba8(AfColor c) {
    uint32_t r = (uint32_t)(af_clamp01(c.r) * 255.0f + 0.5f);
    uint32_t g = (uint32_t)(af_clamp01(c.g) * 255.0f + 0.5f);
    uint32_t b = (uint32_t)(af_clamp01(c.b) * 255.0f + 0.5f);
    uint32_t a = (uint32_t)(af_clamp01(c.a) * 255.0f + 0.5f);
    return (r << 24) | (g << 16) | (b << 8) | a;
}
/** HSV (all 0..1 except h which wraps) -> RGB. */
AF_API AfColor af_color_hsv(float h, float s, float v, float a);

/* ------------------------------------------------------------------- rect */
typedef struct { AfVec2 pos, size; } AfRect;

AF_INLINE AfRect af_rect(float x, float y, float w, float h) {
    AfRect r = {{x, y}, {w, h}};
    return r;
}
AF_INLINE AfRect af_rect_from_minmax(float x0, float y0, float x1, float y1) {
    return af_rect(x0, y0, x1 - x0, y1 - y0);
}
AF_INLINE AfRect af_rect_centered(AfVec2 center, AfVec2 size) {
    return af_rect(center.x - size.x * 0.5f, center.y - size.y * 0.5f, size.x,
                   size.y);
}
AF_INLINE AfRect af_rect_expand(AfRect r, float m) {
    return af_rect(r.pos.x - m, r.pos.y - m, r.size.x + m * 2.0f,
                   r.size.y + m * 2.0f);
}
AF_INLINE float af_rect_left(AfRect r)   { return r.pos.x; }
AF_INLINE float af_rect_top(AfRect r)    { return r.pos.y; }
AF_INLINE float af_rect_right(AfRect r)  { return r.pos.x + r.size.x; }
AF_INLINE float af_rect_bottom(AfRect r) { return r.pos.y + r.size.y; }
AF_INLINE float af_rect_center_x(AfRect r) { return r.pos.x + r.size.x * 0.5f; }
AF_INLINE float af_rect_center_y(AfRect r) { return r.pos.y + r.size.y * 0.5f; }
AF_INLINE AfVec2 af_rect_center(AfRect r) {
    return af_v2(af_rect_center_x(r), af_rect_center_y(r));
}
AF_INLINE int af_rect_contains(AfRect r, AfVec2 p) {
    return p.x >= r.pos.x && p.y >= r.pos.y && p.x <= af_rect_right(r) &&
           p.y <= af_rect_bottom(r);
}
AF_INLINE int af_rect_overlaps(AfRect a, AfRect b) {
    return a.pos.x < af_rect_right(b) && af_rect_right(a) > b.pos.x &&
           a.pos.y < af_rect_bottom(b) && af_rect_bottom(a) > b.pos.y;
}
AF_INLINE AfRect af_rect_intersect(AfRect a, AfRect b) {
    float x0 = af_maxf(af_rect_left(a), af_rect_left(b));
    float y0 = af_maxf(af_rect_top(a), af_rect_top(b));
    float x1 = af_minf(af_rect_right(a), af_rect_right(b));
    float y1 = af_minf(af_rect_bottom(a), af_rect_bottom(b));
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    return af_rect_from_minmax(x0, y0, x1, y1);
}
AF_INLINE AfRect af_rect_union(AfRect a, AfRect b) {
    if (a.size.x <= 0 && a.size.y <= 0) return b;
    if (b.size.x <= 0 && b.size.y <= 0) return a;
    float x0 = af_minf(af_rect_left(a), af_rect_left(b));
    float y0 = af_minf(af_rect_top(a), af_rect_top(b));
    float x1 = af_maxf(af_rect_right(a), af_rect_right(b));
    float y1 = af_maxf(af_rect_bottom(a), af_rect_bottom(b));
    return af_rect_from_minmax(x0, y0, x1, y1);
}
AF_INLINE int af_rect_contains_rect(AfRect outer, AfRect inner) {
    return af_rect_left(inner) >= af_rect_left(outer) &&
           af_rect_top(inner) >= af_rect_top(outer) &&
           af_rect_right(inner) <= af_rect_right(outer) &&
           af_rect_bottom(inner) <= af_rect_bottom(outer);
}
AF_INLINE AfVec2 af_rect_min(AfRect r) { return r.pos; }
AF_INLINE AfVec2 af_rect_max(AfRect r) { return af_v2add(r.pos, r.size); }
AF_INLINE int af_rect_is_empty(AfRect r) { return r.size.x <= 0 || r.size.y <= 0; }
AF_INLINE int af_rect_eq(AfRect a, AfRect b) {
    return a.pos.x == b.pos.x && a.pos.y == b.pos.y && a.size.x == b.size.x &&
           a.size.y == b.size.y;
}
AF_INLINE AfVec2 af_rect_top_left(AfRect r) { return r.pos; }
AF_INLINE AfVec2 af_rect_bottom_right(AfRect r) { return af_rect_max(r); }
AF_INLINE AfVec2 af_rect_top_right(AfRect r) { return af_v2(af_rect_right(r), r.pos.y); }
AF_INLINE AfVec2 af_rect_bottom_left(AfRect r) { return af_v2(r.pos.x, af_rect_bottom(r)); }
/** Grows on all sides, then intersects with the original. */
AF_INLINE AfRect af_rect_clamp_to(AfRect r, AfRect outer) {
    return af_rect_intersect(r, outer);
}

/* ------------------------------------------------------------------- mat3 */
/* Column-major 2D affine transform, applied as m * v. Layout:
 *   | m0 m1 m2 |   | x |
 *   | m3 m4 m5 | * | y |
 *   |  0  0  1 |   | 1 |
 */
typedef struct { float m[9]; } AfMat3;

AF_API AfMat3 af_mat3_identity(void);
AF_API AfMat3 af_mat3_translate(AfVec2 t);
AF_API AfMat3 af_mat3_scale(AfVec2 s);
AF_API AfMat3 af_mat3_rotate(float rad);
AF_API AfMat3 af_mat3_mul(AfMat3 a, AfMat3 b);
/** Compose translate * rotate * scale -- the order you want for a 2D TRS. */
AF_API AfMat3 af_mat3_trs(AfVec2 pos, float rot, AfVec2 scale);
AF_API AfVec2  af_mat3_xform(AfMat3 m, AfVec2 v);
/** Transform a direction (ignores translation). */
AF_API AfVec2  af_mat3_xform_dir(AfMat3 m, AfVec2 v);
AF_API AfMat3  af_mat3_invert(AfMat3 m);
AF_INLINE float af_mat3_det(AfMat3 m) { return m.m[0] * m.m[4] - m.m[3] * m.m[1]; }
/** Lerp every element; for debug interpolation. */
AF_API AfMat3  af_mat3_lerp(AfMat3 a, AfMat3 b, float t);

/* ----------------------------------------------------------------- bounds */
/* Axis-aligned bounding box, kept as min/max for cheap merging. */
typedef struct { AfVec2 min, max; } AfBounds;

AF_API AfBounds af_bounds_empty(void);
AF_API AfBounds af_bounds_from_rect(AfRect r);
AF_API AfBounds af_bounds_from_points(AfVec2* pts, int count);
AF_API AfBounds af_bounds_merge(AfBounds a, AfBounds b);
AF_API AfBounds af_bounds_expand(AfBounds b, AfVec2 by);
AF_API AfBounds af_bounds_transform(AfBounds b, AfMat3 m);
AF_API int      af_bounds_valid(AfBounds b);
AF_API AfRect   af_bounds_rect(AfBounds b);
AF_INLINE AfVec2 af_bounds_center(AfBounds b) { return af_v2scale(af_v2add(b.min, b.max), 0.5f); }
AF_INLINE AfVec2 af_bounds_size(AfBounds b)   { return af_v2sub(b.max, b.min); }
AF_INLINE int     af_bounds_overlap(AfBounds a, AfBounds b) {
    return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y &&
           a.max.y >= b.min.y;
}
AF_INLINE int af_bounds_contains_point(AfBounds b, AfVec2 p) {
    return p.x >= b.min.x && p.x <= b.max.x && p.y >= b.min.y && p.y <= b.max.y;
}

/* -------------------------------------------------------------------- rng */
/* PCG32: small, fast, good enough for gameplay, reproducible across platforms. */
typedef struct { uint64_t state, inc; } AfRng;

AF_API AfRng  af_rng_seed(uint64_t seed);
AF_API AfRng  af_rng_from_entropy(void);
AF_API uint32_t af_rng_u32(AfRng* r);
AF_API int32_t  af_rng_i32(AfRng* r);          /* full range */
AF_API int32_t  af_rng_range(AfRng* r, int lo, int hi); /* inclusive */
AF_API float    af_rng_f32(AfRng* r);          /* [0,1) */
AF_API float    af_rng_range_f(AfRng* r, float lo, float hi);
AF_API float    af_rng_signed(AfRng* r);       /* [-1,1) */
AF_INLINE AfVec2 af_rng_vec2_unit(AfRng* r) {
    float a = af_rng_f32(r) * AF_TAU;
    return af_v2(cosf(a), sinf(a));
}

/* ------------------------------------------------------------------ noise */
/* Value noise with smooth interpolation. Deterministic, seedable. */
AF_API float af_noise1(float x, uint32_t seed);
AF_API float af_noise2(AfVec2 p, uint32_t seed);
/** Fractal brownian motion over af_noise2. Returns roughly -1..1. */
AF_API float af_fbm2(AfVec2 p, uint32_t seed, int octaves, float lacunarity,
                     float gain);

/* ----------------------------------------------------------------- easing */
typedef enum {
    AF_EASE_LINEAR = 0,
    AF_EASE_IN_SINE,   AF_EASE_OUT_SINE,   AF_EASE_INOUT_SINE,
    AF_EASE_IN_QUAD,   AF_EASE_OUT_QUAD,   AF_EASE_INOUT_QUAD,
    AF_EASE_IN_CUBIC,  AF_EASE_OUT_CUBIC,  AF_EASE_INOUT_CUBIC,
    AF_EASE_IN_QUART,  AF_EASE_OUT_QUART,  AF_EASE_INOUT_QUART,
    AF_EASE_IN_EXPO,   AF_EASE_OUT_EXPO,   AF_EASE_INOUT_EXPO,
    AF_EASE_IN_BACK,   AF_EASE_OUT_BACK,   AF_EASE_INOUT_BACK,
    AF_EASE_IN_ELASTIC,AF_EASE_OUT_ELASTIC,AF_EASE_INOUT_ELASTIC,
    AF_EASE_IN_BOUNCE, AF_EASE_OUT_BOUNCE, AF_EASE_INOUT_BOUNCE,
    AF_EASE_COUNT
} AfEase;

/** Apply an easing curve; t is clamped to 0..1, returns the eased progress. */
AF_API float af_ease(AfEase ease, float t);
AF_API const char* af_ease_name(AfEase ease);
AF_API AfEase    af_ease_from_name(const char* name);

#endif /* AFNDLE_CORE_MATH_H */
