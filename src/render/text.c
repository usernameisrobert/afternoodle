/* Text drawing on top of the batched renderer.
 *
 * A string is a run of glyph quads from the font's atlas, so it batches with
 * everything else: a screen full of labels is still one draw call. Newlines
 * are handled here rather than in the font, because line breaking is a
 * drawing concern, not a font one.
 */
#include <SDL.h>

#include "afndle/core/log.h"
#include "afndle/core/math.h"
#include "afndle/core/str.h"
#include "afndle/render/render.h"

#include "font_internal.h"
#include "render_internal.h"

/* Draws one line (no newlines). `pos` is the top-left of the line box. */
static float draw_line(AfRenderer *r, AfFontSize *fs, float scale, AfVec2 pos,
                       const char *text, int len, AfColor c) {
    AfTexture *atlas = af_font_atlas(fs);
    if (!atlas) return 0.0f;
    float baseline = pos.y + fs->metrics.ascent * scale;
    float pen = pos.x;
    int i = 0;
    while (i < len && text[i]) {
        uint32_t cp = af_utf8_next(text, &i);
        if (cp == '\r') continue;
        AfGlyph g;
        if (!af_font_glyph(fs, cp, &g)) break;
        if (!g.blank && g.w > 0 && g.h > 0)
            af_r2d_push_glyph_quad(
                r, atlas, af_v2(pen + g.ox * scale, baseline + g.oy * scale),
                af_v2((float)g.w * scale, (float)g.h * scale),
                af_rect((float)g.x, (float)g.y, (float)g.w, (float)g.h), c);
        pen += g.advance * scale;
    }
    return pen - pos.x;
}

/* Widest line and line count of a multi-line block. */
static void measure_block(AfFontSize *fs, float scale, const char *text,
                          float *out_widest, int *out_lines) {
    float widest = 0.0f, line = 0.0f;
    int lines = 1, i = 0;
    while (text[i]) {
        uint32_t cp = af_utf8_next(text, &i);
        if (cp == '\r') continue;
        if (cp == '\n') {
            if (line > widest) widest = line;
            line = 0.0f;
            lines++;
            continue;
        }
        AfGlyph g;
        af_font_glyph(fs, cp, &g);
        line += g.advance * scale;
    }
    if (line > widest) widest = line;
    *out_widest = widest;
    *out_lines = lines;
}

static void draw_block(AfRenderer *r, AfFontSize *fs, float scale, AfVec2 pos,
                       const char *text, AfColor c) {
    float line_h = fs->metrics.h * scale;
    if (line_h <= 0.0f) line_h = (float)fs->px;
    float y = pos.y;
    int start = 0, i = 0;
    while (text[i]) {
        if (text[i] == '\n') {
            draw_line(r, fs, scale, af_v2(pos.x, y), text + start, i - start, c);
            y += line_h;
            start = i + 1;
        }
        i++;
    }
    draw_line(r, fs, scale, af_v2(pos.x, y), text + start, i - start, c);
}

/* The built-in bitmap font rasterises at one fixed size and scales at draw
 * time, so its scale is not always 1. */
static float size_scale(AfFont *f, float size) {
    if (!f->builtin) return 1.0f;
    float base = (float)fs_builtin_cell_height();
    if (size <= 0.0f || base <= 0.0f) return 1.0f;
    return size / base;
}

void af_r2d_text(AfRenderer *r, AfFont *font, AfVec2 pos, const char *text,
                 AfColor c, float size) {
    if (!r || !font || !text || !*text) return;
    AfFontSize *fs = af_font_size_entry(font, size);
    if (!fs) return;
    draw_block(r, fs, size_scale(font, size), pos, text, c);
}

void af_r2d_text_aligned(AfRenderer *r, AfFont *font, AfVec2 pos, AfAlign h,
                         AfAlign v, const char *text, AfColor c, float size) {
    if (!r || !font || !text || !*text) return;
    AfFontSize *fs = af_font_size_entry(font, size);
    if (!fs) return;
    float scale = size_scale(font, size);
    float widest = 0.0f;
    int lines = 1;
    measure_block(fs, scale, text, &widest, &lines);
    float block_h = fs->metrics.h * scale * (float)lines;

    if (h == AF_ALIGN_CENTER) pos.x -= widest * 0.5f;
    else if (h == AF_ALIGN_RIGHT) pos.x -= widest;
    if (v == AF_ALIGN_MIDDLE) pos.y -= block_h * 0.5f;
    else if (v == AF_ALIGN_BOTTOM) pos.y -= block_h;

    draw_block(r, fs, scale, pos, text, c);
}

void af_r2d_ui_text(AfRenderer *r, AfFont *font, AfVec2 pos, const char *text,
                    AfColor c, float size) {
    if (!r || !font || !text) return;
    /* Same path as world text, with the camera swapped for an identity view.
     * One implementation means the UI cannot drift from the game text. */
    AfCamera2D saved;
    af_r2d_screen_space_begin(r, &saved);
    af_r2d_text(r, font, pos, text, c, size);
    af_r2d_screen_space_end(r, &saved);
}

void af_r2d_ui_text_aligned(AfRenderer *r, AfFont *font, AfVec2 pos, AfAlign h,
                            AfAlign v, const char *text, AfColor c, float size) {
    if (!r || !font || !text) return;
    AfCamera2D saved;
    af_r2d_screen_space_begin(r, &saved);
    af_r2d_text_aligned(r, font, pos, h, v, text, c, size);
    af_r2d_screen_space_end(r, &saved);
}

void af_r2d_text_outline(AfRenderer *r, AfFont *font, AfVec2 pos,
                         const char *text, AfColor c, AfColor outline, float size,
                         float outline_width) {
    if (!r || !font || !text || !*text) return;
    if (outline_width <= 0.0f) outline_width = 1.0f;
    float w = outline_width;
    /* Eight passes around the ring, then the fill on top. Cheaper than
     * generating an outline atlas, and it works for every font. */
    const float dx[8] = {-w, 0.0f, w, -w, w, -w, 0.0f, w};
    const float dy[8] = {-w, -w, -w, 0.0f, 0.0f, w, w, w};
    for (int i = 0; i < 8; i++)
        af_r2d_text(r, font, af_v2add(pos, af_v2(dx[i], dy[i])), text, outline,
                    size);
    af_r2d_text(r, font, pos, text, c, size);
}

float af_r2d_text_width(AfRenderer *r, AfFont *font, const char *text,
                        float size) {
    AF_UNUSED(r);
    if (!font || !text) return 0.0f;
    return af_font_measure(font, size, text, -1);
}

float af_r2d_text_height(AfRenderer *r, AfFont *font, float size) {
    AF_UNUSED(r);
    if (!font) return 0.0f;
    AfFontSize *fs = af_font_size_entry(font, size);
    if (!fs) return 0.0f;
    AfFontMetrics m;
    af_font_get_metrics(font, size, &m);
    return m.h;
}

void af_r2d_text_sized(AfRenderer *r, AfFont *font, AfVec2 pos, const char *text,
                       float width, float max_height, AfColor c) {
    if (!r || !font || !text || !*text) return;
    /* Solve for the size that makes the string exactly `width` wide, then let
     * `max_height` pull it back if that would overflow. */
    float size = 16.0f;
    if (width > 0.0f) {
        float nat = af_font_measure(font, size, text, -1);
        if (nat > 0.0f) size = 16.0f * (width / nat);
    }
    if (max_height > 0.0f) {
        AfFontMetrics m;
        af_font_get_metrics(font, size, &m);
        if (m.h > max_height && m.h > 0.0f) size *= max_height / m.h;
    }
    if (size < 1.0f) size = 1.0f;
    af_r2d_text(r, font, pos, text, c, size);
}
