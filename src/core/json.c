/* JSON DOM. Every node lives in an arena; there is no free, only af_arena_reset.
 *
 * Key order is insertion order, so a parse -> write round trip produces a
 * stable file. This matters: scene and graph files live in the user's project
 * and should produce readable diffs.
 */
#include "afndle/core/json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "afndle/core/log.h"
#include "afndle/core/math.h"
#include "afndle/core/str.h"

/* Keys are compared by content. Objects in engine files have well under 32
 * members, so a linear scan beats the bookkeeping of a hash map, and it keeps
 * the whole DOM a single contiguous arena allocation with no lifetime rules. */

/* ================================================================== parse */

typedef struct {
    const char* p;
    const char* end;
    const char* start;
    AfArena*    arena;
    const char* err;
    int32_t     err_pos;
    int         depth;
} Parser;

#define AF_JSON_MAX_DEPTH 64

static void obj_set(AfArena *a, AfJson *obj, const char *key, AfJson *val);
static AfJson *parse_value(Parser *ps);

static void skip_ws(Parser *ps) {
    while (ps->p < ps->end) {
        char c = *ps->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ps->p++;
        } else if (c == '/' && ps->p + 1 < ps->end && ps->p[1] == '/') {
            while (ps->p < ps->end && *ps->p != '\n') ps->p++;
        } else if (c == '/' && ps->p + 1 < ps->end && ps->p[1] == '*') {
            ps->p += 2;
            while (ps->p + 1 < ps->end && !(*ps->p == '*' && ps->p[1] == '/')) ps->p++;
            ps->p = (ps->p + 2 <= ps->end) ? ps->p + 2 : ps->end;
        } else {
            break;
        }
    }
}

static void fail(Parser *ps, const char *msg) {
    if (!ps->err) {
        ps->err = msg;
        ps->err_pos = (int32_t)(ps->p - ps->start);
    }
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void encode_utf8(AfStrBuf *b, uint32_t cp) {
    if (cp < 0x80) {
        af_strbuf_append_char(b, (char)cp);
    } else if (cp < 0x800) {
        af_strbuf_append_char(b, (char)(0xC0 | (cp >> 6)));
        af_strbuf_append_char(b, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        af_strbuf_append_char(b, (char)(0xE0 | (cp >> 12)));
        af_strbuf_append_char(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        af_strbuf_append_char(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        af_strbuf_append_char(b, (char)(0xF0 | (cp >> 18)));
        af_strbuf_append_char(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
        af_strbuf_append_char(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        af_strbuf_append_char(b, (char)(0x80 | (cp & 0x3F)));
    }
}

static AfJson *parse_string_body(Parser *ps) {
    AF_ASSERT(*ps->p == '"');
    ps->p++;
    /* Fast path: no escapes. */
    const char *run = ps->p;
    while (ps->p < ps->end && *ps->p != '"' && *ps->p != '\\') ps->p++;
    if (ps->p < ps->end && *ps->p == '"') {
        int32_t len = (int32_t)(ps->p - run);
        ps->p++;
        return af_json_new_strn(ps->arena, run, len);
    }

    AfStrBuf out = AF_STRBUF_INIT;
    af_strbuf_append_n(&out, run, (int32_t)(ps->p - run));
    while (ps->p < ps->end) {
        char c = *ps->p;
        if (c == '"') {
            ps->p++;
            AfJson *j = af_json_new_strn(ps->arena, out.data ? out.data : "", out.len);
            af_strbuf_free(&out);
            return j;
        }
        if (c == '\\') {
            ps->p++;
            if (ps->p >= ps->end) break;
            char e = *ps->p++;
            switch (e) {
                case 'n': af_strbuf_append_char(&out, '\n'); break;
                case 't': af_strbuf_append_char(&out, '\t'); break;
                case 'r': af_strbuf_append_char(&out, '\r'); break;
                case 'b': af_strbuf_append_char(&out, '\b'); break;
                case 'f': af_strbuf_append_char(&out, '\f'); break;
                case '/': af_strbuf_append_char(&out, '/'); break;
                case '\\': af_strbuf_append_char(&out, '\\'); break;
                case '"': af_strbuf_append_char(&out, '"'); break;
                case 'u': {
                    if (ps->p + 4 > ps->end) { fail(ps, "truncated \\u escape"); af_strbuf_free(&out); return NULL; }
                    uint32_t cp = 0;
                    for (int i = 0; i < 4; i++) {
                        int h = hex_val(ps->p[i]);
                        if (h < 0) { fail(ps, "bad \\u escape"); af_strbuf_free(&out); return NULL; }
                        cp = (cp << 4) | (uint32_t)h;
                    }
                    ps->p += 4;
                    /* Surrogate pair. */
                    if (cp >= 0xD800 && cp <= 0xDBFF && ps->p + 6 <= ps->end &&
                        ps->p[0] == '\\' && ps->p[1] == 'u') {
                        uint32_t lo = 0;
                        int ok = 1;
                        for (int i = 0; i < 4; i++) {
                            int h = hex_val(ps->p[2 + i]);
                            if (h < 0) { ok = 0; break; }
                            lo = (lo << 4) | (uint32_t)h;
                        }
                        if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            ps->p += 6;
                        }
                    }
                    encode_utf8(&out, cp);
                    break;
                }
                default:
                    fail(ps, "unknown escape sequence");
                    af_strbuf_free(&out);
                    return NULL;
            }
            continue;
        }
        af_strbuf_append_char(&out, c);
        ps->p++;
    }
    fail(ps, "unterminated string");
    af_strbuf_free(&out);
    return NULL;
}

static AfJson *parse_number(Parser *ps) {
    const char *begin = ps->p;
    if (ps->p < ps->end && (*ps->p == '-' || *ps->p == '+')) ps->p++;
    while (ps->p < ps->end) {
        char c = *ps->p;
        if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
            ((c == '-' || c == '+') && ps->p > begin &&
             (ps->p[-1] == 'e' || ps->p[-1] == 'E'))) {
            ps->p++;
            continue;
        }
        break;
    }
    char tmp[64];
    int32_t n = (int32_t)(ps->p - begin);
    if (n <= 0 || n >= (int32_t)sizeof(tmp)) { fail(ps, "bad number"); return NULL; }
    memcpy(tmp, begin, (size_t)n);
    tmp[n] = '\0';
    return af_json_new_num(ps->arena, strtod(tmp, NULL));
}

static AfJson *parse_value(Parser *ps) {
    if (ps->depth > AF_JSON_MAX_DEPTH) { fail(ps, "nesting too deep"); return NULL; }
    skip_ws(ps);
    if (ps->p >= ps->end) { fail(ps, "unexpected end of input"); return NULL; }

    char c = *ps->p;
    AfJson *j = NULL;
    ps->depth++;
    switch (c) {
        case '{': {
            ps->p++;
            j = af_json_new_obj(ps->arena);
            skip_ws(ps);
            if (ps->p < ps->end && *ps->p == '}') { ps->p++; break; }
            for (;;) {
                skip_ws(ps);
                if (ps->p >= ps->end || *ps->p != '"') { fail(ps, "expected object key"); j = NULL; break; }
                AfJson *k = parse_string_body(ps);
                if (!k) { j = NULL; break; }
                skip_ws(ps);
                if (ps->p >= ps->end || *ps->p != ':') { fail(ps, "expected ':'"); j = NULL; break; }
                ps->p++;
                AfJson *v = parse_value(ps);
                if (!v) { j = NULL; break; }
                if (af_json_get(j, k->u.string.data)) {
                    fail(ps, "duplicate object key");
                    j = NULL;
                    break;
                }
                obj_set(ps->arena, j, k->u.string.data, v);
                /* obj_set copies the key into the arena, so the throwaway key
                 * node can die with the next reset. */
                skip_ws(ps);
                if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
                if (ps->p < ps->end && *ps->p == '}') { ps->p++; break; }
                fail(ps, "expected ',' or '}'");
                j = NULL;
                break;
            }
            break;
        }
        case '[': {
            ps->p++;
            j = af_json_new_arr(ps->arena);
            skip_ws(ps);
            if (ps->p < ps->end && *ps->p == ']') { ps->p++; break; }
            for (;;) {
                AfJson *v = parse_value(ps);
                if (!v) { j = NULL; break; }
                af_json_push(ps->arena, j, v);
                skip_ws(ps);
                if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
                if (ps->p < ps->end && *ps->p == ']') { ps->p++; break; }
                fail(ps, "expected ',' or ']'");
                j = NULL;
                break;
            }
            break;
        }
        case '"':
            j = parse_string_body(ps);
            break;
        case 't':
            if (ps->end - ps->p >= 4 && memcmp(ps->p, "true", 4) == 0) { ps->p += 4; j = af_json_new_bool(ps->arena, 1); }
            else fail(ps, "expected 'true'");
            break;
        case 'f':
            if (ps->end - ps->p >= 5 && memcmp(ps->p, "false", 5) == 0) { ps->p += 5; j = af_json_new_bool(ps->arena, 0); }
            else fail(ps, "expected 'false'");
            break;
        case 'n':
            if (ps->end - ps->p >= 4 && memcmp(ps->p, "null", 4) == 0) { ps->p += 4; j = af_json_new_null(ps->arena); }
            else fail(ps, "expected 'null'");
            break;
        default:
            if (c == '-' || c == '+' || (c >= '0' && c <= '9')) {
                j = parse_number(ps);
            } else {
                fail(ps, "unexpected character");
            }
            break;
    }
    ps->depth--;
    return j;
}

AfJson *af_json_parse(AfArena *arena, const char *text, int32_t len,
                      const char **err, int32_t *out_pos) {
    if (err) *err = NULL;
    if (out_pos) *out_pos = 0;
    if (!text) { if (err) *err = "no input"; return NULL; }
    if (len < 0) len = (int32_t)strlen(text);

    Parser ps;
    memset(&ps, 0, sizeof(ps));
    ps.p = text;
    ps.end = text + len;
    ps.start = text;
    ps.arena = arena;

    AfJson *root = parse_value(&ps);
    if (root) {
        skip_ws(&ps);
        if (ps.p != ps.end) {
            fail(&ps, "trailing data after value");
            root = NULL;
        }
    }
    if (!root) {
        if (err) *err = ps.err ? ps.err : "parse error";
        if (out_pos) *out_pos = ps.err_pos;
    }
    return root;
}

/* ============================================================== accessors */

const char *af_json_type_name(AfJsonType t) {
    switch (t) {
        case AF_JSON_NULL: return "null";
        case AF_JSON_BOOL: return "bool";
        case AF_JSON_NUMBER: return "number";
        case AF_JSON_STRING: return "string";
        case AF_JSON_ARRAY: return "array";
        case AF_JSON_OBJECT: return "object";
        default: return "?";
    }
}

int af_json_is(AfJson *j, AfJsonType t) { return j && j->type == t; }

AfJson *af_json_get(AfJson *obj, const char *key) {
    if (!obj || obj->type != AF_JSON_OBJECT || !key) return NULL;
    for (int32_t i = 0; i < obj->u.object.count; i++)
        if (af_str_eq_cstr(obj->u.object.keys[i], key))
            return obj->u.object.vals[i];
    return NULL;
}

AfJson *af_json_at(AfJson *arr, int32_t index) {
    if (!arr || arr->type != AF_JSON_ARRAY) return NULL;
    if (index < 0 || index >= arr->u.array.count) return NULL;
    return arr->u.array.items[index];
}

int32_t af_json_count(AfJson *j) {
    if (!j) return 0;
    if (j->type == AF_JSON_ARRAY) return j->u.array.count;
    if (j->type == AF_JSON_OBJECT) return j->u.object.count;
    return 0;
}

int af_json_get_bool(AfJson *obj, const char *key, int def) {
    AfJson *j = af_json_get(obj, key);
    if (!j) return def;
    if (j->type == AF_JSON_BOOL) return j->u.boolean;
    if (j->type == AF_JSON_NUMBER) return j->u.number != 0.0;
    return def;
}

double af_json_get_f64(AfJson *obj, const char *key, double def) {
    AfJson *j = af_json_get(obj, key);
    return (j && j->type == AF_JSON_NUMBER) ? j->u.number : def;
}

int64_t af_json_get_i64(AfJson *obj, const char *key, int64_t def) {
    AfJson *j = af_json_get(obj, key);
    return (j && j->type == AF_JSON_NUMBER) ? (int64_t)j->u.number : def;
}

int32_t af_json_get_i32(AfJson *obj, const char *key, int32_t def) {
    return (int32_t)af_json_get_i64(obj, key, def);
}

float af_json_get_f32(AfJson *obj, const char *key, float def) {
    return (float)af_json_get_f64(obj, key, (double)def);
}

AfStr af_json_get_str(AfJson *obj, const char *key, const char *def) {
    AfJson *j = af_json_get(obj, key);
    if (j && j->type == AF_JSON_STRING) return af_str_n(j->u.string.data, j->u.string.len);
    return af_str(def);
}

AfJson *af_json_get_arr(AfJson *obj, const char *key) {
    AfJson *j = af_json_get(obj, key);
    return (j && j->type == AF_JSON_ARRAY) ? j : NULL;
}

AfJson *af_json_get_obj(AfJson *obj, const char *key) {
    AfJson *j = af_json_get(obj, key);
    return (j && j->type == AF_JSON_OBJECT) ? j : NULL;
}

int af_json_get_vec2(AfJson *obj, const char *key, float *x, float *y) {
    AfJson *j = af_json_get(obj, key);
    if (!j) return 0;
    if (j->type == AF_JSON_OBJECT) {
        AfJson *jx = af_json_get(j, "x"), *jy = af_json_get(j, "y");
        if (!jx || !jy) return 0;
        *x = (float)(jx->type == AF_JSON_NUMBER ? jx->u.number : 0.0);
        *y = (float)(jy->type == AF_JSON_NUMBER ? jy->u.number : 0.0);
        return 1;
    }
    if (j->type == AF_JSON_ARRAY && j->u.array.count >= 2) {
        AfJson *jx = j->u.array.items[0], *jy = j->u.array.items[1];
        if (!jx || !jy || jx->type != AF_JSON_NUMBER || jy->type != AF_JSON_NUMBER) return 0;
        *x = (float)jx->u.number;
        *y = (float)jy->u.number;
        return 1;
    }
    return 0;
}

int af_json_get_rgba8(AfJson *obj, const char *key, int def[4]) {
    AfJson *j = af_json_get(obj, key);
    if (!j) { if (def) memcpy(def, (int[4]){0, 0, 0, 255}, sizeof(int[4])); return 0; }
    int out[4] = {0, 0, 0, 255};
    if (j->type == AF_JSON_ARRAY) {
        int n = j->u.array.count > 4 ? 4 : j->u.array.count;
        for (int i = 0; i < n; i++) {
            AfJson *it = j->u.array.items[i];
            if (it && it->type == AF_JSON_NUMBER) out[i] = (int)it->u.number;
        }
    } else if (j->type == AF_JSON_OBJECT) {
        out[0] = af_json_get_i32(j, "r", 0);
        out[1] = af_json_get_i32(j, "g", 0);
        out[2] = af_json_get_i32(j, "b", 0);
        out[3] = af_json_get_i32(j, "a", 255);
    } else if (j->type == AF_JSON_NUMBER) {
        uint32_t hex = (uint32_t)j->u.number;
        out[0] = (int)((hex >> 24) & 0xFF);
        out[1] = (int)((hex >> 16) & 0xFF);
        out[2] = (int)((hex >> 8) & 0xFF);
        out[3] = (int)(hex & 0xFF);
    } else {
        return 0;
    }
    for (int i = 0; i < 4; i++) out[i] = af_clampi(out[i], 0, 255);
    if (def) memcpy(def, out, sizeof(int[4]));
    return 1;
}

AfJson *af_json_path(AfJson *root, const char *dotted) {
    if (!root || !dotted) return NULL;
    AfJson *cur = root;
    const char *p = dotted;
    while (*p && cur) {
        const char *dot = strchr(p, '.');
        int32_t len = dot ? (int32_t)(dot - p) : (int32_t)strlen(p);
        cur = af_json_get(cur, af_strndup(p, (size_t)len));
        p = dot ? dot + 1 : p + len;
    }
    return cur;
}

/* =============================================================== mutation */

AfJson *af_json_new(AfArena *a, AfJsonType type) {
    AfJson *j = (AfJson *)af_arena_calloc(a, 1, sizeof(AfJson));
    j->type = type;
    return j;
}

AfJson *af_json_new_null(AfArena *a) { return af_json_new(a, AF_JSON_NULL); }
AfJson *af_json_new_bool(AfArena *a, int v) {
    AfJson *j = af_json_new(a, AF_JSON_BOOL);
    j->u.boolean = v ? 1 : 0;
    return j;
}
AfJson *af_json_new_num(AfArena *a, double v) {
    AfJson *j = af_json_new(a, AF_JSON_NUMBER);
    j->u.number = v;
    return j;
}
AfJson *af_json_new_int(AfArena *a, int64_t v) { return af_json_new_num(a, (double)v); }
AfJson *af_json_new_str(AfArena *a, const char *s) {
    return af_json_new_strn(a, s ? s : "", s ? (int32_t)strlen(s) : 0);
}
AfJson *af_json_new_strn(AfArena *a, const char *s, int32_t len) {
    AfJson *j = af_json_new(a, AF_JSON_STRING);
    char *d = (char *)af_arena_alloc(a, (size_t)len + 1);
    if (len > 0 && s) memcpy(d, s, (size_t)len);
    d[len] = '\0';
    j->u.string.data = d;
    j->u.string.len = len;
    return j;
}
AfJson *af_json_new_arr(AfArena *a) { return af_json_new(a, AF_JSON_ARRAY); }
AfJson *af_json_new_obj(AfArena *a) { return af_json_new(a, AF_JSON_OBJECT); }

void af_json_push(AfArena *a, AfJson *arr, AfJson *val) {
    if (!arr || !val) return;
    if (arr->type == AF_JSON_OBJECT) {
        /* Pushing into an object uses the value's own key, so the same call
         * works whether the caller is building an array or an object. */
        if (val->key.len > 0) obj_set(a, arr, val->key.data, val);
        return;
    }
    if (arr->type != AF_JSON_ARRAY) return;
    if (arr->u.array.count == arr->u.array.cap) {
        int32_t cap = arr->u.array.cap ? arr->u.array.cap * 2 : 4;
        AfJson **items = (AfJson **)af_arena_alloc(a, (size_t)cap * sizeof(AfJson *));
        if (arr->u.array.count)
            memcpy(items, arr->u.array.items, (size_t)arr->u.array.count * sizeof(AfJson *));
        arr->u.array.items = items;
        arr->u.array.cap = cap;
    }
    arr->u.array.items[arr->u.array.count++] = val;
}

static void obj_set(AfArena *a, AfJson *obj, const char *key, AfJson *val) {
    int32_t klen = (int32_t)strlen(key);
    val->key = af_str(af_arena_strndup(a, key, (size_t)klen));
    /* Replace in place to preserve ordering. */
    for (int32_t i = 0; i < obj->u.object.count; i++) {
        if (af_str_eq_cstr(obj->u.object.keys[i], key)) {
            obj->u.object.vals[i] = val;
            return;
        }
    }
    if (obj->u.object.count == obj->u.object.cap) {
        int32_t cap = obj->u.object.cap ? obj->u.object.cap * 2 : 8;
        AfStr *keys = (AfStr *)af_arena_alloc(a, (size_t)cap * sizeof(AfStr));
        AfJson **vals = (AfJson **)af_arena_alloc(a, (size_t)cap * sizeof(AfJson *));
        if (obj->u.object.count) {
            memcpy(keys, obj->u.object.keys, (size_t)obj->u.object.count * sizeof(AfStr));
            memcpy(vals, obj->u.object.vals, (size_t)obj->u.object.count * sizeof(AfJson *));
        }
        obj->u.object.keys = keys;
        obj->u.object.vals = vals;
        obj->u.object.cap = cap;
    }
    int32_t i = obj->u.object.count++;
    obj->u.object.keys[i] = val->key;
    obj->u.object.vals[i] = val;
}

AfJson *af_json_set(AfArena *a, AfJson *obj, const char *key, AfJson *val) {
    if (!obj || obj->type != AF_JSON_OBJECT || !val) return obj;
    obj_set(a, obj, key, val);
    return val;
}

AfJson *af_json_set_bool(AfArena *a, AfJson *obj, const char *key, int v) {
    return af_json_set(a, obj, key, af_json_new_bool(a, v));
}
AfJson *af_json_set_num(AfArena *a, AfJson *obj, const char *key, double v) {
    return af_json_set(a, obj, key, af_json_new_num(a, v));
}
AfJson *af_json_set_int(AfArena *a, AfJson *obj, const char *key, int64_t v) {
    return af_json_set(a, obj, key, af_json_new_int(a, v));
}
AfJson *af_json_set_str(AfArena *a, AfJson *obj, const char *key, const char *v) {
    return af_json_set(a, obj, key, af_json_new_str(a, v));
}
AfJson *af_json_set_f32(AfArena *a, AfJson *obj, const char *key, float v) {
    return af_json_set(a, obj, key, af_json_new_num(a, (double)v));
}
AfJson *af_json_set_vec2(AfArena *a, AfJson *obj, const char *key, float x, float y) {
    AfJson *v = af_json_new_obj(a);
    af_json_set_f32(a, v, "x", x);
    af_json_set_f32(a, v, "y", y);
    return af_json_set(a, obj, key, v);
}
AfJson *af_json_set_rgba8(AfArena *a, AfJson *obj, const char *key, int r, int g, int b, int al) {
    AfJson *v = af_json_new_arr(a);
    af_json_push(a, v, af_json_new_int(a, af_clampi(r, 0, 255)));
    af_json_push(a, v, af_json_new_int(a, af_clampi(g, 0, 255)));
    af_json_push(a, v, af_json_new_int(a, af_clampi(b, 0, 255)));
    af_json_push(a, v, af_json_new_int(a, af_clampi(al, 0, 255)));
    return af_json_set(a, obj, key, v);
}

AfJson *af_json_ensure_obj(AfArena *a, AfJson *obj, const char *key) {
    AfJson *j = af_json_get(obj, key);
    if (j && j->type == AF_JSON_OBJECT) return j;
    AfJson *n = af_json_new_obj(a);
    af_json_set(a, obj, key, n);
    return n;
}

void af_json_remove(AfJson *obj, const char *key) {
    if (!obj || obj->type != AF_JSON_OBJECT) return;
    for (int32_t i = 0; i < obj->u.object.count; i++) {
        if (af_str_eq_cstr(obj->u.object.keys[i], key)) {
            for (int32_t k = i; k < obj->u.object.count - 1; k++) {
                obj->u.object.keys[k] = obj->u.object.keys[k + 1];
                obj->u.object.vals[k] = obj->u.object.vals[k + 1];
            }
            obj->u.object.count--;
            return;
        }
    }
}

/* ================================================================= writing */

static void write_escaped(AfStrBuf *out, const char *s, int32_t len) {
    af_strbuf_append_char(out, '"');
    for (int32_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  af_strbuf_append(out, "\\\""); break;
            case '\\': af_strbuf_append(out, "\\\\"); break;
            case '\n': af_strbuf_append(out, "\\n"); break;
            case '\r': af_strbuf_append(out, "\\r"); break;
            case '\t': af_strbuf_append(out, "\\t"); break;
            case '\b': af_strbuf_append(out, "\\b"); break;
            case '\f': af_strbuf_append(out, "\\f"); break;
            default:
                if (c < 0x20) {
                    char tmp[8];
                    snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                    af_strbuf_append(out, tmp);
                } else {
                    af_strbuf_append_char(out, (char)c);
                }
        }
    }
    af_strbuf_append_char(out, '"');
}

static void write_num(AfStrBuf *out, double v) {
    char tmp[48];
    if (!isfinite(v)) {
        /* JSON has no NaN/Inf. Emit 0 rather than producing an unparseable
         * file; the caller has usually already warned. */
        af_strbuf_append(out, "0");
        return;
    }
    if (v == floor(v) && fabs(v) < 9.0e15) {
        snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
    } else {
        /* Try progressively shorter forms and keep the first that round-trips,
         * so files stay readable without losing precision. */
        for (int prec = 6; prec <= 17; prec++) {
            snprintf(tmp, sizeof(tmp), "%.*g", prec, v);
            if (strtod(tmp, NULL) == v) break;
        }
    }
    af_strbuf_append(out, tmp);
}

static void indent_to(AfStrBuf *out, int level) {
    for (int i = 0; i < level; i++) af_strbuf_append(out, "  ");
}

static void write_value(AfStrBuf *out, AfJson *j, int level, int pretty) {
    if (!j) { af_strbuf_append(out, "null"); return; }
    switch (j->type) {
        case AF_JSON_NULL: af_strbuf_append(out, "null"); break;
        case AF_JSON_BOOL: af_strbuf_append(out, j->u.boolean ? "true" : "false"); break;
        case AF_JSON_NUMBER: write_num(out, j->u.number); break;
        case AF_JSON_STRING: write_escaped(out, j->u.string.data, j->u.string.len); break;
        case AF_JSON_ARRAY: {
            if (j->u.array.count == 0) { af_strbuf_append(out, "[]"); break; }
            af_strbuf_append_char(out, '[');
            for (int32_t i = 0; i < j->u.array.count; i++) {
                if (i) af_strbuf_append_char(out, ',');
                if (pretty) { af_strbuf_append_char(out, '\n'); indent_to(out, level + 1); }
                write_value(out, j->u.array.items[i], level + 1, pretty);
            }
            if (pretty) { af_strbuf_append_char(out, '\n'); indent_to(out, level); }
            af_strbuf_append_char(out, ']');
            break;
        }
        case AF_JSON_OBJECT: {
            if (j->u.object.count == 0) { af_strbuf_append(out, "{}"); break; }
            af_strbuf_append_char(out, '{');
            for (int32_t i = 0; i < j->u.object.count; i++) {
                if (i) af_strbuf_append_char(out, ',');
                if (pretty) { af_strbuf_append_char(out, '\n'); indent_to(out, level + 1); }
                AfStr k = j->u.object.keys[i];
                write_escaped(out, k.data, k.len);
                af_strbuf_append_char(out, ':');
                if (pretty) af_strbuf_append_char(out, ' ');
                write_value(out, j->u.object.vals[i], level + 1, pretty);
            }
            if (pretty) { af_strbuf_append_char(out, '\n'); indent_to(out, level); }
            af_strbuf_append_char(out, '}');
            break;
        }
        default: af_strbuf_append(out, "null"); break;
    }
}

void af_json_write(AfJson *root, AfStrBuf *out) { write_value(out, root, 0, 0); }
void af_json_write_pretty(AfJson *root, AfStrBuf *out, int indent) {
    (void)indent;
    write_value(out, root, 0, 1);
}

char *af_json_to_string(AfArena *a, AfJson *root, int pretty) {
    AfStrBuf sb = AF_STRBUF_INIT;
    if (pretty) af_json_write_pretty(root, &sb, 2);
    else af_json_write(root, &sb);
    char *out = af_arena_strndup(a, sb.data ? sb.data : "{}", (size_t)sb.len);
    af_strbuf_free(&sb);
    return out;
}

void af_json_write_file(AfJson *root, const char *path, int pretty) {
    AfStrBuf sb = AF_STRBUF_INIT;
    if (pretty) af_json_write_pretty(root, &sb, 2);
    else af_json_write(root, &sb);
    af_strbuf_append_char(&sb, '\n');
    FILE *f = fopen(path, "wb");
    if (!f) {
        AF_WARN("could not write '%s'", path);
    } else {
        fwrite(sb.data, 1, (size_t)sb.len, f);
        fclose(f);
    }
    af_strbuf_free(&sb);
}

/* ==================================================================== file */

AfJson *af_json_parse_file(AfArena *arena, const char *path, const char **err) {
    if (err) *err = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) { if (err) *err = "file not found"; return NULL; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); if (err) *err = "could not stat file"; return NULL; }
    char *buf = (char *)af_arena_alloc(arena, (size_t)size + 1);
    size_t got = fread(buf, 1, (size_t)size, f);
    buf[got] = '\0';
    fclose(f);
    return af_json_parse(arena, buf, (int32_t)got, err, NULL);
}
