/* Minimal JSON DOM parser + writer.
 *
 * Every parse takes an AfArena* so the whole tree dies in one reset. The writer
 * pretty-prints with stable key order (insertion order), which keeps scene and
 * graph files diff-friendly in version control.
 */
#ifndef AFNDLE_CORE_JSON_H
#define AFNDLE_CORE_JSON_H

#include <stdint.h>
#include "afndle/core/afconfig.h"
#include "afndle/core/mem.h"
#include "afndle/core/str.h"

typedef enum {
    AF_JSON_NULL = 0,
    AF_JSON_BOOL,
    AF_JSON_NUMBER,
    AF_JSON_STRING,
    AF_JSON_ARRAY,
    AF_JSON_OBJECT
} AfJsonType;

typedef struct AfJson AfJson;

struct AfJson {
    AfJsonType type;
    AfStr      key;    /* member name when inside an object */
    union {
        int   boolean;
        double number;
        struct { char* data; int32_t len; } string; /* NUL-terminated */
        struct { AfJson** items; int32_t count, cap; } array;
        struct {
            AfStr*    keys;  /* member names, NUL-terminated in the arena */
            AfJson**  vals;
            int32_t   count, cap;
        } object;
    } u;
};

/* Parse `len` bytes (or -1 for strlen). Returns NULL and fills `err` on bad
 * input; *out_pos receives the byte offset of the first error. */
AF_API AfJson* af_json_parse(AfArena* arena, const char* text, int32_t len,
                             const char** err, int32_t* out_pos);
AF_API AfJson* af_json_parse_file(AfArena* arena, const char* path,
                                  const char** err);
AF_API const char* af_json_type_name(AfJsonType t);

/* ------------------------------------------------------------- accessors */
AF_API AfJson*  af_json_get(AfJson* obj, const char* key);
AF_API AfJson*  af_json_at(AfJson* arr, int32_t index);
AF_API int32_t  af_json_count(AfJson* j);
AF_API int      af_json_is(AfJson* j, AfJsonType t);

/* Typed getters with defaults. Never crash on NULL or type mismatch. */
AF_API int     af_json_get_bool(AfJson* obj, const char* key, int def);
AF_API int32_t af_json_get_i32(AfJson* obj, const char* key, int32_t def);
AF_API int64_t af_json_get_i64(AfJson* obj, const char* key, int64_t def);
AF_API float   af_json_get_f32(AfJson* obj, const char* key, float def);
AF_API double  af_json_get_f64(AfJson* obj, const char* key, double def);
AF_API AfStr   af_json_get_str(AfJson* obj, const char* key, const char* def);
AF_API AfJson* af_json_get_arr(AfJson* obj, const char* key);
AF_API AfJson* af_json_get_obj(AfJson* obj, const char* key);
/** v as {x,y} or as a 2-element array. Returns 0 on type mismatch. */
AF_API int     af_json_get_vec2(AfJson* obj, const char* key, float* x, float* y);
/** v as {r,g,b,a} with 0..255 ints, or {r,g,b} with a=def. */
AF_API int     af_json_get_rgba8(AfJson* obj, const char* key, int def[4]);

/* ------------------------------------------------------------- mutation */
AF_API AfJson* af_json_new(AfArena* a, AfJsonType type);
AF_API AfJson* af_json_new_null(AfArena* a);
AF_API AfJson* af_json_new_bool(AfArena* a, int v);
AF_API AfJson* af_json_new_num(AfArena* a, double v);
AF_API AfJson* af_json_new_int(AfArena* a, int64_t v);
AF_API AfJson* af_json_new_str(AfArena* a, const char* s);
AF_API AfJson* af_json_new_strn(AfArena* a, const char* s, int32_t len);
AF_API AfJson* af_json_new_arr(AfArena* a);
AF_API AfJson* af_json_new_obj(AfArena* a);

/** Sets obj[key]. Creates the key if missing. Ownership passes to the parent. */
AF_API AfJson* af_json_set(AfArena* a, AfJson* obj, const char* key, AfJson* val);
AF_API AfJson* af_json_set_bool(AfArena* a, AfJson* obj, const char* key, int v);
AF_API AfJson* af_json_set_num(AfArena* a, AfJson* obj, const char* key, double v);
AF_API AfJson* af_json_set_int(AfArena* a, AfJson* obj, const char* key, int64_t v);
AF_API AfJson* af_json_set_str(AfArena* a, AfJson* obj, const char* key, const char* v);
AF_API AfJson* af_json_set_f32(AfArena* a, AfJson* obj, const char* key, float v);
AF_API AfJson* af_json_set_vec2(AfArena* a, AfJson* obj, const char* key, float x, float y);
AF_API AfJson* af_json_set_rgba8(AfArena* a, AfJson* obj, const char* key, int r, int g, int b, int al);
AF_API void    af_json_push(AfArena* a, AfJson* arr, AfJson* val);
/** Removes a key from an object, shifting the rest down. */
AF_API void    af_json_remove(AfJson* obj, const char* key);

/* --------------------------------------------------------------- writing */
/** Compact one-line output. */
AF_API void af_json_write(AfJson* root, AfStrBuf* out);
/** Indented output, two spaces per level. */
AF_API void af_json_write_pretty(AfJson* root, AfStrBuf* out, int indent);
AF_API void af_json_write_file(AfJson* root, const char* path, int pretty);
AF_API char* af_json_to_string(AfArena* a, AfJson* root, int pretty);

/* ------------------------------------------------------------ path sugar */
/** Reads a dotted path like "player.health.current". Missing parts -> NULL. */
AF_API AfJson* af_json_path(AfJson* root, const char* dotted);
/** Ensures obj[key] exists as an object, creating intermediates. */
AF_API AfJson* af_json_ensure_obj(AfArena* a, AfJson* obj, const char* key);

#endif /* AFNDLE_CORE_JSON_H */
