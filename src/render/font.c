/* Fonts: TrueType with a per-size glyph atlas, plus the built-in bitmap font.
 *
 * A font rasterises each codepoint on first use into a shelf-packed atlas and
 * remembers where it went. A UI has two or three sizes and a few hundred
 * glyphs, so this stays small and every glyph is pixel-sharp at the size the
 * user asked for -- no scaling a big atlas, no blurry UI text.
 *
 * The atlas is kept as a CPU mirror so a new glyph is a partial
 * SDL_UpdateTexture instead of a full re-upload.
 */
#include <SDL.h>
#include <SDL_ttf.h>

#include "afndle/core/log.h"
#include "afndle/core/mem.h"
#include "afndle/core/str.h"
#include "afndle/platform/platform.h"
#include "afndle/render/builtinfont.h"
#include "afndle/render/font.h"
#include "afndle/render/texture.h"

#include "font_internal.h"

/* A 512x512 RGBA atlas is 1MB and holds thousands of UI glyphs. */
#define AF_ATLAS_SIZE 512

/* ==================================================================== utf8 */

uint32_t af_utf8_next(const char *s, int *i) {
    if (!s || !i) return 0;
    const unsigned char *p = (const unsigned char *)s + *i;
    unsigned char c = p[0];
    int extra, cp;
    if (c == 0) return 0;
    if (c < 0x80) { cp = c; extra = 0; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else { /* stray continuation byte: skip it so we always make progress */
        *i += 1;
        return 0xFFFD;
    }
    for (int k = 1; k <= extra; k++) {
        unsigned char cc = p[k];
        if ((cc & 0xC0) != 0x80) { *i += 1; return 0xFFFD; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    *i += extra + 1;
    return cp;
}

int af_utf8_encode(uint32_t cp, char out[4]) {
    if (!out) return 0;
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

int af_utf8_length(const char *s) {
    if (!s) return 0;
    int n = 0;
    for (int i = 0; s[i];) {
        int before = i;
        af_utf8_next(s, &i);
        if (i <= before) i = before + 1;
        n++;
    }
    return n;
}

/* ============================================================ built-in font */

const uint8_t *af_builtin_font_glyph(uint32_t cp) {
    const AfBuiltinFont *b = &af_builtin_font;
    if (cp >= b->first_codepoint && cp <= b->last_codepoint) {
        uint32_t idx = cp - b->first_codepoint;
        return b->ascii_glyphs + (size_t)idx * AF_BUILTIN_GLYPH_COLS;
    }
    for (uint32_t i = 0; i < b->extra_count; i++)
        if (b->extra_codepoints[i] == cp)
            return b->extra_glyphs + (size_t)i * AF_BUILTIN_GLYPH_COLS;
    return NULL;
}

/* The built-in font's atlas is built once for the whole process: a strip of
 * 6x8 cells, ink stored as white with the generator's alpha. */
static AfFontSize *g_builtin_size;

static int builtin_build_atlas(AfFontSize *fs) {
    const AfBuiltinFont *b = &af_builtin_font;
    uint32_t count = (b->last_codepoint - b->first_codepoint + 1) + b->extra_count;
    int gw = (int)b->cell_w, gh = (int)b->cell_h;
    int w = (int)count * gw, h = gh;
    /* Keep it a power of two wide so the GPU likes it. */
    int pw = 1;
    while (pw < w) pw *= 2;

    Uint32 *px = (Uint32 *)af_calloc((size_t)pw * (size_t)h, 4);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t cp = (i < count - b->extra_count)
                          ? b->first_codepoint + i
                          : b->extra_codepoints[i - (count - b->extra_count)];
        const uint8_t *cols = af_builtin_font_glyph(cp);
        if (!cols) continue;
        int ox = (int)i * gw;
        for (int col = 0; col < AF_BUILTIN_GLYPH_COLS; col++)
            for (int row = 0; row < AF_BUILTIN_GLYPH_ROWS; row++)
                if (cols[col] & (1u << row))
                    px[(size_t)row * pw + ox + col] = 0xFFFFFFFFu;
    }
    fs->cpu = px;
    fs->atlas = af_texture_create(px, pw, h, pw * 4, AF_FILTER_NEAREST);
    fs->pen_x = (int)count * gw;
    fs->pen_y = h;
    fs->row_h = h;
    return fs->atlas != NULL;
}

static AfFont *g_builtin_font;

AfFont *af_font_builtin(void) {
    if (g_builtin_font) return g_builtin_font;
    g_builtin_font = (AfFont *)af_calloc(1, sizeof(AfFont));
    g_builtin_font->builtin = 1;
    g_builtin_font->size = 16.0f;
    af_str_cpy_max(af_str("<builtin>"), g_builtin_font->path,
                   (int)sizeof(g_builtin_font->path));

    g_builtin_size = (AfFontSize *)af_calloc(1, sizeof(AfFontSize));
    g_builtin_size->font = g_builtin_font;
    g_builtin_size->font_builtin = 1;
    g_builtin_size->px = af_builtin_font.cell_h;
    g_builtin_size->metrics.w = af_builtin_font.cell_w;
    g_builtin_size->metrics.h = af_builtin_font.cell_h;
    g_builtin_size->metrics.ascent = af_builtin_font.glyph_h;
    g_builtin_size->metrics.descent =
        (float)(af_builtin_font.cell_h - af_builtin_font.glyph_h);
    g_builtin_size->table_cap = 256;
    g_builtin_size->table =
        (AfGlyph *)af_calloc((size_t)g_builtin_size->table_cap, sizeof(AfGlyph));
    g_builtin_size->table_count = 0;
    g_builtin_font->sizes = g_builtin_size;
    if (!builtin_build_atlas(g_builtin_size))
        AF_WARN("could not build the built-in font atlas");
    return g_builtin_font;
}

/* ================================================================ true type */

/* Fonts to try, best first, when nothing is configured. The names are what
 * desktop Linux, macOS, Android and Windows boxes actually ship. */
static const char *const k_font_candidates[] = {
    "DejaVuSans.ttf", "LiberationSans-Regular.ttf", "NotoSans-Regular.ttf",
    "FreeSans.ttf",   "Ubuntu-R.ttf",            "Roboto-Regular.ttf",
    "Arial.ttf",      "segoeui.ttf",             "Helvetica.ttc",
    "Arial Unicode.ttf", "Geneva.ttf",           "SFNS.ttf",
};

static int file_is_ttf(const char *path) {
    const char *ext = af_fs_extension(path);
    if (af_str_casecmp(af_str(ext), ".ttf") == 0) return 1;
    if (af_str_casecmp(af_str(ext), ".otf") == 0) return 1;
    if (af_str_casecmp(af_str(ext), ".ttc") == 0) return 1;
    return 0;
}

static int scan_dir_for_fonts(const char *dir, char paths[][512], int max,
                              int count) {
    if (count >= max) return count;
    int n = 0;
    AfDirEntry *entries = af_fs_list_dir(dir, &n);
    if (!entries) return count;
    for (int i = 0; i < n && count < max; i++) {
        char full[512];
        af_fs_path_join(dir, entries[i].name, full, (int)sizeof(full));
        if (entries[i].is_dir || !file_is_ttf(full)) continue;
        af_str_cpy_max(af_str(full), paths[count], 512);
        count++;
    }
    af_free(entries);
    return count;
}

int af_font_enumerate(char paths[][512], int max) {
    int count = 0;
    if (!paths || max <= 0) return 0;
    const char *env = SDL_getenv("AFNDLE_FONT_DIR");
    if (env && *env) count = scan_dir_for_fonts(env, paths, max, count);

    static const char *const k_dirs[] = {
#if defined(AF_OS_ANDROID)
        "/system/fonts", "/data/fonts", "/usr/share/fonts",
#elif defined(AF_OS_MACOS)
        "/System/Library/Fonts", "/Library/Fonts", "/System/Library/Fonts/Supplemental",
#elif defined(AF_OS_WINDOWS)
        "C:/Windows/Fonts",
#else
        "/usr/share/fonts/truetype/dejavu", "/usr/share/fonts/truetype",
        "/usr/share/fonts", "/usr/local/share/fonts", "~/.fonts",
        "~/.local/share/fonts",
#endif
    };
    for (size_t i = 0; i < sizeof(k_dirs) / sizeof(k_dirs[0]) && count < max; i++)
        count = scan_dir_for_fonts(k_dirs[i], paths, max, count);
    return count;
}

static int init_ttf_once(void);

static const char *find_system_font(char *out, int cap) {
    const char *forced = SDL_getenv("AFNDLE_FONT");
    if (forced && *forced && af_fs_is_file(forced)) {
        af_str_cpy_max(af_str(forced), out, cap);
        return out;
    }
    /* A font shipped next to the executable wins: a project can drop one in. */
    char exe[512];
    af_fs_exe_dir(exe, (int)sizeof(exe));
    char local[512];
    af_fs_path_join(exe, "font.ttf", local, (int)sizeof(local));
    if (af_fs_is_file(local)) {
        af_str_cpy_max(af_str(local), out, cap);
        return out;
    }
    af_fs_path_join(exe, "assets/font.ttf", local, (int)sizeof(local));
    if (af_fs_is_file(local)) {
        af_str_cpy_max(af_str(local), out, cap);
        return out;
    }

    static char (*found)[512] = NULL;
    static int found_n = -1;
    if (found_n < 0) {
        found_n = 256;
        found = (char (*)[512])af_calloc((size_t)found_n, 512);
        int n = af_font_enumerate(found, found_n);
        if (n < found_n) found_n = n;
    }
    for (int i = 0; i < found_n; i++) {
        for (size_t k = 0; k < sizeof(k_font_candidates) / sizeof(char *); k++) {
            const char *want = k_font_candidates[k];
            const char *p = found[i] + strlen(found[i]);
            while (p > found[i] && p[-1] != '/' && p[-1] != '\\') p--;
            if (af_str_casecmp(af_str(p), want) == 0) {
                af_str_cpy_max(af_str(found[i]), out, cap);
                return out;
            }
        }
    }
    if (found_n > 0) {
        af_str_cpy_max(af_str(found[0]), out, cap);
        return out;
    }
    return NULL;
}

AfFont *af_font_load(const char *path, float size) {
    if (!path || !*path) return NULL;
    if (size <= 0.0f) size = 16.0f;
    if (!init_ttf_once()) {
        AF_WARN("SDL_ttf unavailable; falling back to the built-in font");
        AfFont *f = af_font_builtin();
        f->size = size;
        return f;
    }
    TTF_Font *ttf = TTF_OpenFont(path, (int)size);
    if (!ttf) {
        AF_WARN("could not open font '%s': %s", path, TTF_GetError());
        return NULL;
    }
    /* Hinting at small sizes is what makes UI text readable; the hint style is
     * a hint, so ignore a failure on exotic builds. */
    TTF_SetFontHinting(ttf, TTF_HINTING_NORMAL);
    TTF_SetFontKerning(ttf, 1);

    AfFont *f = (AfFont *)af_calloc(1, sizeof(AfFont));
    f->ttf = ttf;
    f->size = size;
    af_str_cpy_max(af_str(path), f->path, (int)sizeof(f->path));
    return f;
}

AfFont *af_font_default(void) {
    static AfFont *cached;
    if (cached) return cached;
    if (!init_ttf_once()) return af_font_builtin();

    char path[512];
    if (find_system_font(path, (int)sizeof(path))) {
        cached = af_font_load(path, 16.0f);
        if (cached) return cached;
        AF_WARN("found '%s' but could not load it; using the built-in font",
                path);
    } else {
        AF_INFO("no system font found; using the built-in font");
    }
    cached = af_font_builtin();
    return cached;
}

static int init_ttf_once(void) {
    static int state = -1; /* -1 unknown, 0 failed, 1 ready */
    if (state >= 0) return state;
    if (TTF_Init() != 0) {
        AF_WARN("TTF_Init failed: %s", TTF_GetError());
        state = 0;
    } else {
        state = 1;
    }
    return state;
}

int af_font_is_builtin(const AfFont *f) { return f ? f->builtin : 1; }
const char *af_font_path(const AfFont *f) { return f ? f->path : ""; }
float af_font_size(const AfFont *f) { return f ? f->size : 0.0f; }

void af_font_destroy(AfFont *f) {
    /* The built-in font is a process singleton: the editor and every window
     * share it, so it outlives individual users. */
    if (!f || f->builtin) return;
    for (AfFontSize *fs = f->sizes; fs;) {
        AfFontSize *next = fs->next;
        af_free(fs->table);
        af_free(fs->cpu);
        if (fs->atlas) af_texture_destroy(fs->atlas);
        af_free(fs);
        fs = next;
    }
    if (f->ttf) TTF_CloseFont(f->ttf);
    af_free(f);
}

void af_font_purge_atlases(AfFont *f) {
    if (!f) return;
    for (AfFontSize *fs = f->sizes; fs; fs = fs->next) {
        if (fs->atlas) af_texture_destroy(fs->atlas);
        fs->atlas = NULL;
        af_free(fs->cpu);
        fs->cpu = NULL;
        af_free(fs->table);
        fs->table = NULL;
        fs->table_cap = fs->table_count = 0;
        fs->pen_x = fs->pen_y = fs->row_h = 0;
    }
}

/* ============================================================= atlas glyphs */

static int table_find(AfFontSize *fs, uint32_t cp) {
    if (!fs->table_cap) return -1;
    uint32_t mask = (uint32_t)fs->table_cap - 1u;
    uint32_t i = (uint32_t)af_hash_u32(cp) & mask;
    for (uint32_t probe = 0; probe <= mask; probe++) {
        if (!fs->table[i].drawn) return -(int)i - 1; /* empty slot */
        if (fs->table[i].cp == cp) return (int)i;
        i = (i + 1u) & mask;
    }
    return -1;
}

static void table_grow(AfFontSize *fs) {
    int new_cap = fs->table_cap ? fs->table_cap * 2 : 256;
    AfGlyph *old = fs->table;
    int old_cap = fs->table_cap;
    fs->table = (AfGlyph *)af_calloc((size_t)new_cap, sizeof(AfGlyph));
    fs->table_cap = new_cap;
    fs->table_count = 0;
    for (int i = 0; i < old_cap; i++) {
        if (!old[i].drawn) continue;
        int slot = table_find(fs, old[i].cp);
        if (slot < 0) fs->table[-slot - 1] = old[i];
    }
    af_free(old);
}

static AfGlyph *table_insert(AfFontSize *fs, uint32_t cp) {
    if (fs->table_count * 4 >= fs->table_cap * 3) table_grow(fs);
    int slot = table_find(fs, cp);
    if (slot >= 0) return &fs->table[slot];
    if (!slot) return NULL;
    AfGlyph *g = &fs->table[-slot - 1];
    memset(g, 0, sizeof(*g));
    g->cp = cp;
    g->drawn = 1;
    fs->table_count++;
    return g;
}

AfTexture *af_font_atlas(AfFontSize *fs) { return fs ? fs->atlas : NULL; }

int fs_builtin_cell_height(void) { return (int)af_builtin_font.cell_h; }

/* Reserves a w x h box in the shelf packer. Returns 0 when the atlas is full. */
static int shelf_reserve(AfFontSize *fs, int w, int h, int *out_x, int *out_y) {
    if (w <= 0 || h <= 0) return 0;
    w++; /* 1px gutter so bilinear filtering never bleeds between glyphs */
    h++;
    if (fs->pen_x + w > AF_ATLAS_SIZE) {
        fs->pen_x = 0;
        fs->pen_y += fs->row_h;
        fs->row_h = 0;
    }
    if (fs->pen_y + h > AF_ATLAS_SIZE) return 0;
    *out_x = fs->pen_x;
    *out_y = fs->pen_y;
    fs->pen_x += w;
    if (h > fs->row_h) fs->row_h = h;
    return 1;
}

/* Uploads a glyph's pixels into the atlas. The source is white with coverage
 * in alpha, so the draw-time tint colours it. */
static void atlas_blit(AfFontSize *fs, int x, int y, int w, int h,
                       const SDL_Surface *src, int sx, int sy) {
    const Uint32 *sp = (const Uint32 *)src->pixels;
    int sp_stride = src->pitch / 4;
    for (int row = 0; row < h; row++) {
        Uint32 *dst = fs->cpu + (size_t)(y + row) * AF_ATLAS_SIZE + x;
        const Uint32 *srow = sp + (size_t)(sy + row) * sp_stride + sx;
        for (int col = 0; col < w; col++) {
            Uint8 a = (Uint8)(srow[col] >> 24);
            dst[col] = ((Uint32)a << 24) | 0x00FFFFFFu;
        }
    }
    SDL_Rect dirty = {x, y, w, h};
    SDL_UpdateTexture((SDL_Texture *)af_texture_sdl(fs->atlas), &dirty,
                      fs->cpu + (size_t)y * AF_ATLAS_SIZE + x,
                      AF_ATLAS_SIZE * 4);
}

static void builtin_glyph(AfFontSize *fs, uint32_t cp, AfGlyph *g) {
    const AfBuiltinFont *b = &af_builtin_font;
    int gw = (int)b->cell_w, gh = (int)b->cell_h;
    int idx = -1;
    uint32_t count = b->last_codepoint - b->first_codepoint + 1;
    if (cp >= b->first_codepoint && cp <= b->last_codepoint)
        idx = (int)(cp - b->first_codepoint);
    else
        for (uint32_t i = 0; i < b->extra_count; i++)
            if (b->extra_codepoints[i] == cp) idx = (int)(count + i);

    g->advance = (float)gw;
    g->ox = 0.0f;
    g->oy = -(float)b->glyph_h;
    if (idx < 0) { g->blank = 1; return; }

    const uint8_t *cols = af_builtin_font_glyph(cp);
    if (!cols) { g->blank = 1; return; }

    int x = idx * gw, y = 0;
    g->x = (int16_t)x;
    g->y = (int16_t)y;
    g->w = (int16_t)b->glyph_w;
    g->h = (int16_t)b->glyph_h;
    g->blank = 0;
    AF_UNUSED(gh);
}

static void ttf_glyph(AfFont *f, AfFontSize *fs, uint32_t cp, AfGlyph *g) {
    char utf8[4];
    int n = af_utf8_encode(cp, utf8);
    utf8[n] = '\0';

    /* Every size shares one TTF_Font, and its point size is global mutable
     * state. Pin it to this size first or we would silently rasterise a glyph
     * at whatever size happened to be requested last. */
    TTF_SetFontSize(f->ttf, fs->px);

    /* Metrics first: SDL2 surfaces carry no sub-pixel origin, so the pen
     * offset has to come from the glyph metrics. */
    int minx = 0, maxx = 0, miny = 0, maxy = 0, advance = 0;
    if (TTF_GlyphMetrics32(f->ttf, cp, &minx, &maxx, &miny, &maxy, &advance) != 0) {
        g->blank = 1;
        return;
    }
    g->advance = (float)advance;
    g->ox = (float)minx;
    g->oy = -(float)maxy;

    SDL_Color white = {255, 255, 255, 255};
    SDL_Surface *surf = TTF_RenderUTF8_Blended(f->ttf, utf8, white);
    if (!surf) { g->blank = 1; return; }

    int sw = surf->w, sh = surf->h;
    if (sw <= 0 || sh <= 0) { /* whitespace */
        g->blank = 1;
        SDL_FreeSurface(surf);
        return;
    }
    if (sw > af_texture_width(fs->atlas) || sh > af_texture_height(fs->atlas)) {
        SDL_FreeSurface(surf);
        g->blank = 1;
        return;
    }
    int ax = 0, ay = 0;
    if (!shelf_reserve(fs, sw, sh, &ax, &ay)) {
        if (!fs->full_warned) {
            AF_WARN("font atlas full at %dpx; some glyphs will not render",
                    fs->px);
            fs->full_warned = 1;
        }
        SDL_FreeSurface(surf);
        g->blank = 1;
        return;
    }
    atlas_blit(fs, ax, ay, sw, sh, surf, 0, 0);
    SDL_FreeSurface(surf);

    g->x = (int16_t)ax;
    g->y = (int16_t)ay;
    g->w = (int16_t)sw;
    g->h = (int16_t)sh;
    g->blank = 0;
}

int af_font_glyph(AfFontSize *fs, uint32_t cp, AfGlyph *out) {
    if (!fs || !out) return 0;
    int slot = table_find(fs, cp);
    if (slot >= 0) {
        *out = fs->table[slot];
        return 1;
    }
    if (!fs->atlas) return 0;

    AfGlyph *g = table_insert(fs, cp);
    if (!g) return 0;
    if (cp == '\n' || cp == '\r' || cp == '\t') {
        g->advance = fs->metrics.w * (cp == '\t' ? 4.0f : 0.0f);
        g->blank = 1;
    } else if (fs->font_builtin) {
        builtin_glyph(fs, cp, g);
    } else {
        ttf_glyph(fs->font, fs, cp, g);
    }
    *out = *g;
    return 1;
}

AfFontSize *af_font_size_entry(AfFont *f, float size) {
    if (!f) return NULL;
    if (size <= 0.0f) size = f->size;
    if (f->builtin) return f->sizes;

    int px = (int)(size + 0.5f);
    if (px < 1) px = 1;
    for (AfFontSize *fs = f->sizes; fs; fs = fs->next)
        if (fs->px == px) return fs;

    if (TTF_SetFontSize(f->ttf, px) != 0) {
        AF_WARN("could not set font size %d: %s", px, TTF_GetError());
        return NULL;
    }

    AfFontSize *fs = (AfFontSize *)af_calloc(1, sizeof(AfFontSize));
    fs->font = f;
    fs->font_builtin = 0;
    fs->px = px;
    fs->cpu = (Uint32 *)af_calloc((size_t)AF_ATLAS_SIZE * AF_ATLAS_SIZE, 4);
    fs->atlas = af_texture_create(fs->cpu, AF_ATLAS_SIZE, AF_ATLAS_SIZE,
                                  AF_ATLAS_SIZE * 4, AF_FILTER_LINEAR);
    if (!fs->atlas) {
        af_free(fs->cpu);
        af_free(fs);
        return NULL;
    }
    fs->table_cap = 256;
    fs->table =
        (AfGlyph *)af_calloc((size_t)fs->table_cap, sizeof(AfGlyph));
    fs->next = f->sizes;
    f->sizes = fs;

    /* SDL_ttf 2.24 reports integer metrics; the fractional accessors landed
     * in SDL3, so scale these when the caller wants a fractional size. */
    int ascent = TTF_FontAscent(f->ttf);
    int descent = TTF_FontDescent(f->ttf);
    int line_skip = TTF_FontLineSkip(f->ttf);
    if (ascent <= 0) ascent = px * 4 / 5;
    if (line_skip <= 0) line_skip = px;
    fs->metrics.ascent = (float)ascent;
    fs->metrics.descent = (float)(descent > 0 ? descent : px - ascent);
    fs->metrics.h = (float)line_skip;
    fs->metrics.w = fs->metrics.h * 0.5f; /* rough, refined per glyph */
    return fs;
}

int af_font_request_size(AfFont *f, float size) {
    AfFontSize *fs = af_font_size_entry(f, size);
    return fs != NULL;
}

void af_font_get_metrics(AfFont *f, float size, AfFontMetrics *out) {
    if (!out) return;
    *out = (AfFontMetrics){0, 0, 0, 0};
    if (!f) return;
    AfFontSize *fs = af_font_size_entry(f, size);
    if (!fs) return;
    *out = fs->metrics;
    if (f->builtin && size > 0.0f) {
        /* The bitmap cell is the only truth; scale it to the requested line
         * height so the built-in font tracks the same size argument. */
        float s = size / (float)af_builtin_font.cell_h;
        out->w = af_builtin_font.cell_w * s;
        out->h = af_builtin_font.cell_h * s;
        out->ascent = af_builtin_font.glyph_h * s;
        out->descent = (float)(af_builtin_font.cell_h - af_builtin_font.glyph_h) * s;
    }
}

/* ============================================================ text measuring */

float af_font_measure(AfFont *f, float size, const char *text, int max_chars) {
    if (!f || !text) return 0.0f;
    AfFontSize *fs = af_font_size_entry(f, size);
    if (!fs) return 0.0f;
    float scale = 1.0f;
    if (f->builtin) {
        if (size <= 0.0f) return 0.0f;
        scale = size / (float)af_builtin_font.cell_h;
    }

    float widest = 0.0f, line = 0.0f;
    int i = 0, n = 0;
    while (text[i] && (max_chars <= 0 || n < max_chars)) {
        uint32_t cp = af_utf8_next(text, &i);
        if (cp == '\r') continue;
        if (cp == '\n') {
            if (line > widest) widest = line;
            line = 0.0f;
            n++;
            continue;
        }
        AfGlyph g;
        af_font_glyph(fs, cp, &g);
        line += g.advance * scale;
        n++;
    }
    if (line > widest) widest = line;
    return widest;
}

int af_font_char_at(AfFont *f, float size, const char *text, float x,
                    int *out_byte_offset) {
    if (out_byte_offset) *out_byte_offset = 0;
    if (!f || !text || !*text) return 0;
    AfFontSize *fs = af_font_size_entry(f, size);
    if (!fs) return 0;
    float scale = 1.0f;
    if (f->builtin && size > 0.0f)
        scale = size / (float)af_builtin_font.cell_h;

    /* Walk the string tracking the pen, and stop at the glyph whose midpoint
     * `x` falls in. The returned index counts characters, not bytes. */
    int i = 0, index = 0;
    float pen = 0.0f;
    while (text[i]) {
        int start = i;
        uint32_t cp = af_utf8_next(text, &i);
        if (cp == '\r') continue;
        if (cp == '\n') {
            if (x <= pen) { if (out_byte_offset) *out_byte_offset = start; return index; }
            pen = 0.0f;
            index++;
            continue;
        }
        AfGlyph g;
        af_font_glyph(fs, cp, &g);
        float mid = pen + g.advance * scale * 0.5f;
        if (x < mid) {
            if (out_byte_offset) *out_byte_offset = start;
            return index;
        }
        pen += g.advance * scale;
        index++;
    }
    if (out_byte_offset) *out_byte_offset = i;
    return index;
}

int af_font_ellipsize(AfFont *f, float size, const char *text, float max_w,
                      char *out, int out_size) {
    if (!out || out_size <= 0) return 0;
    out[0] = '\0';
    if (!text) return 0;
    int total = af_utf8_length(text);
    if (total == 0) return 0;

    const char *ellipsis = "\xe2\x80\xa6"; /* U+2026 */
    float ew = af_font_measure(f, size, ellipsis, -1);
    if (af_font_measure(f, size, text, -1) <= max_w) {
        af_str_cpy_max(af_str(text), out, out_size);
        return total;
    }
    if (ew > max_w) {
        /* Not even the ellipsis fits: give the first character or nothing. */
        int i = 0;
        af_utf8_next(text, &i);
        out[0] = '\0';
        if (i > 0 && i < out_size) memcpy(out, text, (size_t)i);
        return i > 0 ? 1 : 0;
    }

    /* Longest prefix that leaves room for the ellipsis. */
    int lo = 0, hi = total, cut = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        char tmp[512];
        if (mid > 0) {
            int i = 0;
            for (int k = 0; k < mid; k++) af_utf8_next(text, &i);
            af_str_cpy_max(af_str_n(text, i), tmp, (int)sizeof(tmp));
        } else {
            tmp[0] = '\0';
        }
        if (af_font_measure(f, size, tmp, -1) + ew <= max_w) {
            cut = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    int i = 0;
    for (int k = 0; k < cut; k++) af_utf8_next(text, &i);
    size_t used = (size_t)(i < out_size ? i : out_size - 1);
    if (used > 0) memcpy(out, text, used);
    if (used + 3 < (size_t)out_size) memcpy(out + used, ellipsis, 3);
    else out[used] = '\0';
    out[used + (used + 3 < (size_t)out_size ? 3 : 0)] = '\0';
    return cut + 1;
}
