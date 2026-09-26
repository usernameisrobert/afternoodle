/* The engine's built-in 5x7 bitmap font. See scripts/gen_font.py.
 *
 * Every glyph is 5x7 ink inside a 6x8 cell (one byte per column, LSB is the
 * top row). This is the last-resort font: the renderer prefers a real TTF
 * found at runtime and falls back to this, so text always renders.
 */
#ifndef AFNDLE_RENDER_BUILTINFONT_H
#define AFNDLE_RENDER_BUILTINFONT_H

#include <stdint.h>

#include "afndle/core/afconfig.h"

#define AF_BUILTIN_GLYPH_COLS 5
#define AF_BUILTIN_GLYPH_ROWS 7

typedef struct {
    uint8_t glyph_w, glyph_h;   /* ink size inside the cell */
    uint8_t cell_w,  cell_h;    /* advance box, includes 1px spacing */
    uint32_t first_codepoint;   /* first glyph of ascii_glyphs */
    uint32_t last_codepoint;
    /* (last_codepoint - first_codepoint + 1) rows of 5 column bytes. */
    const uint8_t* ascii_glyphs;
    uint32_t extra_count;
    const uint32_t* extra_codepoints;
    const uint8_t* extra_glyphs; /* extra_count rows of 5 column bytes */
} AfBuiltinFont;

extern const AfBuiltinFont af_builtin_font;

/** Named codepoints the editor UI draws. Private use area, so they can never
 *  collide with real text the user types. */
#define AF_GLYPH_BOX     0x01u  /* rounded box outline          */
#define AF_GLYPH_CROSS   0x02u  /* plus in a box: collapse      */
#define AF_GLYPH_CHEVRON 0x03u  /* right chevron: expand        */
#define AF_GLYPH_TEE     0x04u  /* tee: branch from the left    */
#define AF_GLYPH_ARROW   0x05u  /* arrow: drag/flow output      */

/** Returns the 5 column bytes for `cp`, or NULL if the font has no glyph. */
AF_API const uint8_t* af_builtin_font_glyph(uint32_t codepoint);

#endif /* AFNDLE_RENDER_BUILTINFONT_H */
