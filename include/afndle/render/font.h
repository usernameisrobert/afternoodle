/* Fonts: TrueType with a per-size glyph atlas, plus the built-in bitmap font.
 *
 * Glyphs are rasterised on demand into shelf-packed atlases, one per integer
 * pixel size. A UI has two or three sizes and a few hundred glyphs, so this
 * stays small while always being pixel sharp at the size you asked for.
 */
#ifndef AFNDLE_RENDER_FONT_H
#define AFNDLE_RENDER_FONT_H

#include "afndle/core/afconfig.h"
#include "afndle/core/math.h"
#include "afndle/core/str.h"

typedef struct AfFont AfFont;
typedef struct AfRenderer AfRenderer;

typedef struct {
    float  w, h;          /* advance and line height in pixels */
    float  ascent, descent;
} AfFontMetrics;

AF_API AfFont* af_font_load(const char* path, float size);
/** Discovers a usable system font. Set AFNDLE_FONT to force a specific file.
 *  Falls back to the built-in bitmap font; never returns NULL. */
AF_API AfFont* af_font_default(void);
AF_API AfFont* af_font_builtin(void);
AF_API void    af_font_destroy(AfFont* f);
AF_API int     af_font_is_builtin(const AfFont* f);
AF_API const char* af_font_path(const AfFont* f);
AF_API float   af_font_size(const AfFont* f);
/** Rasterises (or reuses) the size you ask for. Cheap; the atlas is cached. */
AF_API int     af_font_request_size(AfFont* f, float size);
AF_API void    af_font_get_metrics(AfFont* f, float size, AfFontMetrics* out);
/** UTF-8 aware advance, and the width of the widest line for multiline text.
 *  `max_chars` caps how many characters are counted; 0 or less means all. */
AF_API float   af_font_measure(AfFont* f, float size, const char* text,
                               int max_chars);
/** Index of the character nearest `x` pixels into the string, and the byte
 *  offset of that character. Used for text hit-testing in the editor. */
AF_API int     af_font_char_at(AfFont* f, float size, const char* text, float x,
                               int* out_byte_offset);
/** Truncates to fit `max_w`, appending an ellipsis when it has to cut. */
AF_API int     af_font_ellipsize(AfFont* f, float size, const char* text,
                                 float max_w, char* out, int out_size);
/** Releases every cached atlas. Call when memory is tight. */
AF_API void    af_font_purge_atlases(AfFont* f);
/** Lists TTF files found on this machine, for the editor's font picker. */
AF_API int     af_font_enumerate(char paths[][512], int max);
/** Decodes UTF-8 to the next codepoint, advancing *i. */
AF_API uint32_t af_utf8_next(const char* s, int* i);
AF_API int     af_utf8_encode(uint32_t cp, char out[4]);
AF_API int     af_utf8_length(const char* s);

#endif /* AFNDLE_RENDER_FONT_H */
