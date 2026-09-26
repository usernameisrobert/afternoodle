/* Private font internals shared by font.c and text.c. */
#ifndef AFNDLE_RENDER_FONT_INTERNAL_H
#define AFNDLE_RENDER_FONT_INTERNAL_H

#include <SDL_ttf.h>

#include "afndle/render/font.h"

/* A rasterised glyph: where it lives in the atlas, and how the pen moves. */
typedef struct {
    uint32_t cp;
    int16_t  x, y, w, h;   /* atlas rect in atlas pixels */
    float    advance;      /* pen advance */
    float    ox, oy;       /* quad top-left relative to (pen, baseline) */
    int      blank;        /* whitespace: no ink, but still advances */
    int      drawn;        /* 0 = an empty slot in the hash table */
} AfGlyph;

/* One pixel size of one font. Glyphs are shelf-packed into a single atlas. */
typedef struct AfFontSize {
    struct AfFontSize* next;
    AfFont*    font;                 /* owning font */
    int        font_builtin;         /* rasterise from the bitmap table */
    int        px;                   /* the integer size this is for */
    AfFontMetrics metrics;
    AfTexture*   atlas;
    Uint32*      cpu;                /* mirrors the atlas, ABGR8888 */
    int          pen_x, pen_y, row_h;
    AfGlyph*     table;
    int          table_cap;          /* power of two */
    int          table_count;
    int          full_warned;
} AfFontSize;

struct AfFont {
    TTF_Font* ttf;          /* NULL for the built-in bitmap font */
    char      path[512];
    float     size;         /* default size, pixels */
    int       builtin;
    AfFontSize* sizes;      /* linked list, most recent first */
};

/* Returns the cached size entry for `size`, rasterising on demand. Never
 * returns NULL for a valid font: falls back to the nearest existing size. */
AfFontSize* af_font_size_entry(AfFont* f, float size);
/* Looks up (rasterising if needed) one glyph. Returns 0 if the font has no
 * such codepoint at all; `out` is still filled with a blank advance. */
int         af_font_glyph(AfFontSize* fs, uint32_t cp, AfGlyph* out);
AfTexture*  af_font_atlas(AfFontSize* fs);
/* Line height of the built-in font's cell, the thing world sizes are measured
 * against. Exposed so text.c can scale bitmap glyphs the same way. */
int         fs_builtin_cell_height(void);

#endif /* AFNDLE_RENDER_FONT_INTERNAL_H */
