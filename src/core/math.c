#include "afndle/core/math.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "afndle/core/time.h"

/* ================================================================= colour */

AfColor af_color_hsv(float h, float s, float v, float a) {
    h = af_wrapf(h, 0.0f, 1.0f);
    s = af_clamp01(s);
    v = af_clamp01(v);
    float i = floorf(h * 6.0f);
    float f = h * 6.0f - i;
    float p = v * (1.0f - s);
    float q = v * (1.0f - s * f);
    float t = v * (1.0f - s * (1.0f - f));
    int seg = ((int)i) % 6;
    if (seg < 0) seg += 6;
    float r, g, b;
    switch (seg) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
    return af_color(r, g, b, a);
}

/* =================================================================== mat3 */

AfMat3 af_mat3_identity(void) {
    AfMat3 r = {{1, 0, 0, 0, 1, 0, 0, 0, 1}};
    return r;
}

AfMat3 af_mat3_translate(AfVec2 t) {
    AfMat3 r = {{1, 0, t.x, 0, 1, t.y, 0, 0, 1}};
    return r;
}

AfMat3 af_mat3_scale(AfVec2 s) {
    AfMat3 r = {{s.x, 0, 0, 0, s.y, 0, 0, 0, 1}};
    return r;
}

AfMat3 af_mat3_rotate(float rad) {
    float c = cosf(rad), s = sinf(rad);
    AfMat3 r = {{c, -s, 0, s, c, 0, 0, 0, 1}};
    return r;
}

/* Row-major (see the header): r[i][j] = sum_k a[i][k] * b[k][j]. */
AfMat3 af_mat3_mul(AfMat3 a, AfMat3 b) {
    AfMat3 r;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            r.m[i * 3 + j] = a.m[i * 3 + 0] * b.m[0 * 3 + j] +
                             a.m[i * 3 + 1] * b.m[1 * 3 + j] +
                             a.m[i * 3 + 2] * b.m[2 * 3 + j];
    return r;
}

AfMat3 af_mat3_trs(AfVec2 pos, float rot, AfVec2 scale) {
    /* T * R * S, row-major. Note the scale is applied per axis before the
     * rotation, which is what makes a rotated non-uniform sprite shear the
     * way you expect. */
    float c = cosf(rot), s = sinf(rot);
    AfMat3 r = {{c * scale.x, -s * scale.y, pos.x,
                s * scale.x,  c * scale.y, pos.y,
                0.0f,        0.0f,        1.0f}};
    return r;
}

AfVec2 af_mat3_xform(AfMat3 m, AfVec2 v) {
    return af_v2(m.m[0] * v.x + m.m[1] * v.y + m.m[2],
                 m.m[3] * v.x + m.m[4] * v.y + m.m[5]);
}

AfVec2 af_mat3_xform_dir(AfMat3 m, AfVec2 v) {
    return af_v2(m.m[0] * v.x + m.m[1] * v.y, m.m[3] * v.x + m.m[4] * v.y);
}

AfMat3 af_mat3_invert(AfMat3 m) {
    float det = af_mat3_det(m);
    AfMat3 r;
    if (af_absf(det) < 1e-12f) return af_mat3_identity();
    float id = 1.0f / det;
    r.m[0] = (m.m[4] * m.m[8] - m.m[5] * m.m[7]) * id;
    r.m[1] = (m.m[2] * m.m[7] - m.m[1] * m.m[8]) * id;
    r.m[2] = (m.m[1] * m.m[5] - m.m[2] * m.m[4]) * id;
    r.m[3] = (m.m[5] * m.m[6] - m.m[3] * m.m[8]) * id;
    r.m[4] = (m.m[0] * m.m[8] - m.m[2] * m.m[6]) * id;
    r.m[5] = (m.m[2] * m.m[3] - m.m[0] * m.m[5]) * id;
    r.m[6] = (m.m[3] * m.m[7] - m.m[4] * m.m[6]) * id;
    r.m[7] = (m.m[1] * m.m[6] - m.m[0] * m.m[7]) * id;
    r.m[8] = (m.m[0] * m.m[4] - m.m[1] * m.m[3]) * id;
    return r;
}

AfMat3 af_mat3_lerp(AfMat3 a, AfMat3 b, float t) {
    AfMat3 r;
    for (int i = 0; i < 9; i++) r.m[i] = af_lerpf(a.m[i], b.m[i], t);
    return r;
}

/* ================================================================= bounds */

AfBounds af_bounds_empty(void) {
    AfBounds b;
    b.min = af_v2(AF_FLT_MAX, AF_FLT_MAX);
    b.max = af_v2(-AF_FLT_MAX, -AF_FLT_MAX);
    return b;
}

AfBounds af_bounds_from_rect(AfRect r) {
    AfBounds b;
    b.min = r.pos;
    b.max = af_v2add(r.pos, r.size);
    return b;
}

AfBounds af_bounds_from_points(AfVec2 *pts, int count) {
    AfBounds b = af_bounds_empty();
    for (int i = 0; i < count; i++) {
        b.min = af_v2min(b.min, pts[i]);
        b.max = af_v2max(b.max, pts[i]);
    }
    return b;
}

AfBounds af_bounds_merge(AfBounds a, AfBounds b) {
    AfBounds r;
    r.min = af_v2min(a.min, b.min);
    r.max = af_v2max(a.max, b.max);
    return r;
}

AfBounds af_bounds_expand(AfBounds b, AfVec2 by) {
    b.min = af_v2sub(b.min, by);
    b.max = af_v2add(b.max, by);
    return b;
}

AfBounds af_bounds_transform(AfBounds b, AfMat3 m) {
    /* Transform all four corners: exact for AABBs under any affine map. */
    AfVec2 corners[4] = {b.min, af_v2(b.max.x, b.min.y), b.max,
                         af_v2(b.min.x, b.max.y)};
    AfBounds out = af_bounds_empty();
    for (int i = 0; i < 4; i++) out = af_bounds_merge(out, af_bounds_from_points(&corners[i], 1));
    return out;
}

int af_bounds_valid(AfBounds b) { return b.min.x <= b.max.x && b.min.y <= b.max.y; }

AfRect af_bounds_rect(AfBounds b) {
    return af_rect_from_minmax(b.min.x, b.min.y, b.max.x, b.max.y);
}

/* ==================================================================== rng */
/* PCG-XSH-RR 64/32. State: 64 bits, stream increment: 64 bits. */

AfRng af_rng_seed(uint64_t seed) {
    AfRng r;
    r.state = 0u;
    r.inc = (seed << 1u) | 1u;
    /* Warm up. */
    af_rng_u32(&r);
    r.state += 0x853c49e6748fea9bULL + seed;
    af_rng_u32(&r);
    return r;
}

AfRng af_rng_from_entropy(void) {
    /* Mix several cheap entropy sources. Good enough for a game seed. */
    uint64_t h = (uint64_t)af_time_ms();
    h ^= (uint64_t)(uintptr_t)&h << 13;
    h ^= (uint64_t)(af_time_now() * 1000000.0);
    h *= 0x9E3779B97F4A7C15ULL;
    return af_rng_seed(h);
}

uint32_t af_rng_u32(AfRng *r) {
    uint64_t old = r->state;
    r->state = old * 6364136223846793005ULL + (r->inc | 1u);
    uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
    uint32_t rot = (uint32_t)(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31));
}

int32_t af_rng_i32(AfRng *r) { return (int32_t)af_rng_u32(r); }

int32_t af_rng_range(AfRng *r, int lo, int hi) {
    if (hi <= lo) return lo;
    uint32_t span = (uint32_t)(hi - lo) + 1u;
    return lo + (int32_t)(af_rng_u32(r) % span);
}

float af_rng_f32(AfRng *r) { return (float)(af_rng_u32(r) >> 8) * (1.0f / 16777216.0f); }

float af_rng_range_f(AfRng *r, float lo, float hi) {
    return lo + (hi - lo) * af_rng_f32(r);
}

float af_rng_signed(AfRng *r) { return af_rng_f32(r) * 2.0f - 1.0f; }

/* ================================================================== noise */

static inline float hash2i(int32_t x, int32_t y, uint32_t seed) {
    uint32_t h = (uint32_t)x * 0x8DA6B343u ^ (uint32_t)y * 0xD8163841u ^ seed;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return (float)(h & 0xFFFFFFu) * (1.0f / 16777216.0f);
}

float af_noise1(float x, uint32_t seed) {
    int32_t i = (int32_t)floorf(x);
    float f = x - (float)i;
    float a = hash2i(i, 0, seed);
    float b = hash2i(i + 1, 0, seed);
    return af_lerpf(a, b, af_smoothstep(f)) * 2.0f - 1.0f;
}

float af_noise2(AfVec2 p, uint32_t seed) {
    int32_t ix = (int32_t)floorf(p.x), iy = (int32_t)floorf(p.y);
    float fx = p.x - (float)ix, fy = p.y - (float)iy;
    float ux = af_smoothstep(fx), uy = af_smoothstep(fy);
    float a = hash2i(ix, iy, seed);
    float b = hash2i(ix + 1, iy, seed);
    float c = hash2i(ix, iy + 1, seed);
    float d = hash2i(ix + 1, iy + 1, seed);
    float top = af_lerpf(a, b, ux);
    float bot = af_lerpf(c, d, ux);
    return af_lerpf(top, bot, uy) * 2.0f - 1.0f;
}

float af_fbm2(AfVec2 p, uint32_t seed, int octaves, float lacunarity, float gain) {
    if (octaves < 1) octaves = 1;
    if (lacunarity <= 0.0f) lacunarity = 2.0f;
    if (gain <= 0.0f) gain = 0.5f;
    float sum = 0.0f, amp = 1.0f, norm = 0.0f, freq = 1.0f;
    for (int i = 0; i < octaves; i++) {
        sum += af_noise2(af_v2scale(p, freq), seed + (uint32_t)i * 7919u) * amp;
        norm += amp;
        amp *= gain;
        freq *= lacunarity;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

/* ================================================================== easing */

static const char* const k_ease_names[AF_EASE_COUNT] = {
    "Linear", "InSine",  "OutSine",   "InOutSine",  "InQuad",   "OutQuad",
    "InOutQuad", "InCubic", "OutCubic", "InOutCubic", "InQuart",  "OutQuart",
    "InOutQuart", "InExpo", "OutExpo",  "InOutExpo",  "InBack",   "OutBack",
    "InOutBack", "InElastic", "OutElastic", "InOutElastic", "InBounce",
    "OutBounce", "InOutBounce"};

static float ease_out_bounce(float t) {
    const float n1 = 7.5625f, d1 = 2.75f;
    if (t < 1.0f / d1) return n1 * t * t;
    if (t < 2.0f / d1) { t -= 1.5f / d1; return n1 * t * t + 0.75f; }
    if (t < 2.5f / d1) { t -= 2.25f / d1; return n1 * t * t + 0.9375f; }
    t -= 2.625f / d1;
    return n1 * t * t + 0.984375f;
}

static float ease_in_bounce(float t) { return 1.0f - ease_out_bounce(1.0f - t); }
static float ease_in_out_bounce(float t) {
    if (t < 0.5f) return (1.0f - ease_out_bounce(1.0f - t * 2.0f)) * 0.5f;
    return (1.0f + ease_out_bounce(t * 2.0f - 1.0f)) * 0.5f;
}

static float ease_out_elastic(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    const float c4 = AF_TAU / 3.0f;
    return powf(2.0f, -10.0f * t) * sinf((t * 10.0f - 0.75f) * c4) + 1.0f;
}

static float ease_in_back(float t, float s) {
    return t * t * ((s + 1.0f) * t - s);
}
static float ease_out_back(float t, float s) {
    float u = t - 1.0f;
    return u * u * ((s + 1.0f) * u + s) + 1.0f;
}
static float ease_in_out_back(float t, float s) {
    float c = s * 1.525f;
    if (t < 0.5f) return 0.5f * (2.0f * t) * (2.0f * t * ((c + 1.0f) * 2.0f * t - c));
    float u = 2.0f * t - 2.0f;
    return 0.5f * (u * u * ((c + 1.0f) * u + c) + 2.0f);
}

float af_ease(AfEase ease, float t) {
    t = af_clamp01(t);
    switch (ease) {
        case AF_EASE_LINEAR:        return t;
        case AF_EASE_IN_SINE:       return 1.0f - cosf(t * AF_HALF_PI);
        case AF_EASE_OUT_SINE:      return sinf(t * AF_HALF_PI);
        case AF_EASE_INOUT_SINE:    return 0.5f * (1.0f - cosf(t * AF_PI));
        case AF_EASE_IN_QUAD:       return t * t;
        case AF_EASE_OUT_QUAD:      return 1.0f - (1.0f - t) * (1.0f - t);
        case AF_EASE_INOUT_QUAD:
            return t < 0.5f ? 2.0f * t * t : 1.0f - 2.0f * (1.0f - t) * (1.0f - t);
        case AF_EASE_IN_CUBIC:      return t * t * t;
        case AF_EASE_OUT_CUBIC:     return 1.0f - powf(1.0f - t, 3.0f);
        case AF_EASE_INOUT_CUBIC:
            return t < 0.5f ? 4.0f * t * t * t : 1.0f - powf(2.0f * t - 2.0f, 3.0f) / 2.0f;
        case AF_EASE_IN_QUART:      return t * t * t * t;
        case AF_EASE_OUT_QUART:     return 1.0f - powf(1.0f - t, 4.0f);
        case AF_EASE_INOUT_QUART:
            return t < 0.5f ? 8.0f * t * t * t * t : 1.0f - 8.0f * powf(1.0f - t, 4.0f);
        case AF_EASE_IN_EXPO:       return t <= 0.0f ? 0.0f : powf(2.0f, 10.0f * t - 10.0f);
        case AF_EASE_OUT_EXPO:      return t >= 1.0f ? 1.0f : 1.0f - powf(2.0f, -10.0f * t);
        case AF_EASE_INOUT_EXPO:
            if (t <= 0.0f) return 0.0f;
            if (t >= 1.0f) return 1.0f;
            return t < 0.5f ? 0.5f * powf(2.0f, 20.0f * t - 10.0f)
                           : 1.0f - 0.5f * powf(2.0f, -20.0f * t + 10.0f);
        case AF_EASE_IN_BACK:       return ease_in_back(t, 1.70158f);
        case AF_EASE_OUT_BACK:      return ease_out_back(t, 1.70158f);
        case AF_EASE_INOUT_BACK:    return ease_in_out_back(t, 1.70158f);
        case AF_EASE_IN_ELASTIC:    return 1.0f - ease_out_elastic(1.0f - t);
        case AF_EASE_OUT_ELASTIC:   return ease_out_elastic(t);
        case AF_EASE_INOUT_ELASTIC:
            return t < 0.5f ? 0.5f * (1.0f - ease_out_elastic(1.0f - 2.0f * t))
                           : 0.5f * (1.0f + ease_out_elastic(2.0f * t - 1.0f));
        case AF_EASE_IN_BOUNCE:     return ease_in_bounce(t);
        case AF_EASE_OUT_BOUNCE:    return ease_out_bounce(t);
        case AF_EASE_INOUT_BOUNCE:  return ease_in_out_bounce(t);
        default:                    return t;
    }
}

const char *af_ease_name(AfEase e) {
    if ((int)e < 0 || (int)e >= AF_EASE_COUNT) return "Linear";
    return k_ease_names[e];
}

AfEase af_ease_from_name(const char *name) {
    if (!name) return AF_EASE_LINEAR;

    /* Normalise both sides: lowercase, separators dropped. So "InOutSine",
     * "in_out_sine" and "in-out-sine" all match. */
    char want[32];
    int w = 0;
    for (const char *p = name; *p && w < (int)sizeof(want) - 1; p++) {
        char c = *p;
        if (c == '_' || c == '-' || c == ' ') continue;
        want[w++] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    want[w] = '\0';

    for (int i = 0; i < AF_EASE_COUNT; i++) {
        const char *src = k_ease_names[i];
        char have[32];
        int h = 0;
        for (; src[h] && h < (int)sizeof(have) - 1; h++) {
            char c = src[h];
            have[h] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
        }
        have[h] = '\0';
        if (strcmp(have, want) == 0) return (AfEase)i;
    }
    return AF_EASE_LINEAR;
}
