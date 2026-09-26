/* String helpers: borrowed views, owned buffers, small text utilities.
 *
 * C has no string type, so the engine standardises on:
 *   AfStr    - borrowed, non-owning (ptr + len). Not required to be NUL-terminated.
 *   AfStrBuf - owned, growable, always NUL-terminated.
 */
#ifndef AFNDLE_CORE_STR_H
#define AFNDLE_CORE_STR_H

#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "afndle/core/afconfig.h"
#include "afndle/core/mem.h"

/* ------------------------------------------------------------- AfStr view */
typedef struct {
    const char* data;
    int32_t     len;
} AfStr;

AF_INLINE AfStr af_str(const char* cstr) {
    AfStr s;
    s.data = cstr;
    s.len = cstr ? (int32_t)strlen(cstr) : 0;
    return s;
}
AF_INLINE AfStr af_str_n(const char* ptr, int32_t len) {
    AfStr s;
    s.data = ptr;
    s.len = (ptr && len > 0) ? len : 0;
    return s;
}
/* Compile-time literal: af_str_lit("abc") == af_str_n("abc", 3) */
#define af_str_lit(lit) af_str_n("" lit, (int32_t)(sizeof(lit) - 1))
AF_INLINE int af_str_eq(AfStr a, AfStr b) {
    if (a.data == b.data && a.len == b.len) return 1;
    if (a.len != b.len || a.len == 0) return 0;
    return memcmp(a.data, b.data, (size_t)a.len) == 0;
}
AF_INLINE int af_str_eq_cstr(AfStr a, const char* cstr) {
    return af_str_eq(a, af_str(cstr));
}
AF_INLINE int af_str_empty(AfStr a) { return a.len <= 0; }
AF_INLINE int af_str_starts_with(AfStr s, AfStr prefix) {
    if (prefix.len > s.len) return 0;
    return memcmp(s.data, prefix.data, (size_t)prefix.len) == 0;
}
AF_INLINE int af_str_ends_with(AfStr s, AfStr suffix) {
    if (suffix.len > s.len) return 0;
    return memcmp(s.data + (s.len - suffix.len), suffix.data, (size_t)suffix.len) == 0;
}
AF_INLINE int af_str_contains(AfStr s, char c) {
    return s.len > 0 && memchr(s.data, c, (size_t)s.len) != NULL;
}
AF_INLINE int af_str_find_char(AfStr s, char c) {
    for (int32_t i = 0; i < s.len; i++)
        if (s.data[i] == c) return i;
    return -1;
}
AF_INLINE int af_str_find(AfStr hay, AfStr needle) {
    if (needle.len <= 0) return 0;
    if (needle.len > hay.len) return -1;
    for (int32_t i = 0; i + needle.len <= hay.len; i++)
        if (memcmp(hay.data + i, needle.data, (size_t)needle.len) == 0) return i;
    return -1;
}
AF_INLINE AfStr af_str_slice(AfStr s, int32_t start, int32_t end) {
    if (start < 0) start = 0;
    if (end > s.len) end = s.len;
    if (end < start) end = start;
    return af_str_n(s.data + start, end - start);
}
/** Copy into a caller buffer, always NUL-terminated. Needs len+1 bytes. */
AF_INLINE void af_str_cpy(AfStr s, char* out) {
    if (s.len > 0) memcpy(out, s.data, (size_t)s.len);
    out[s.len] = '\0';
}
/** Like af_str_cpy but truncates to fit `out_size` (always NUL-terminated). */
AF_INLINE void af_str_cpy_max(AfStr s, char* out, int32_t out_size) {
    if (out_size <= 0) return;
    int32_t n = s.len < out_size - 1 ? s.len : out_size - 1;
    if (n > 0) memcpy(out, s.data, (size_t)n);
    out[n] = '\0';
}
AF_INLINE int af_str_to_f32(AfStr s, float* out) {
    char tmp[64];
    af_str_cpy_max(s, tmp, (int32_t)sizeof(tmp));
    char* endp = NULL;
    float v = strtof(tmp, &endp);
    if (endp == tmp) return 0;
    *out = v;
    return 1;
}
AF_INLINE int af_str_to_i32(AfStr s, int32_t* out) {
    char tmp[64];
    af_str_cpy_max(s, tmp, (int32_t)sizeof(tmp));
    char* endp = NULL;
    long v = strtol(tmp, &endp, 10);
    if (endp == tmp) return 0;
    *out = (int32_t)v;
    return 1;
}
/** Drops leading/trailing whitespace in place via a view. */
AF_API AfStr af_str_trim(AfStr s);
/** Copy into a caller buffer, always NUL-terminated, truncating if needed. */
/* --------------------------------------------------------- AfStrBuf (own) */
typedef struct {
    char*  data;
    int32_t len;
    int32_t cap;
} AfStrBuf;

#define AF_STRBUF_INIT {NULL, 0, 0}

AF_API void  af_strbuf_init(AfStrBuf* b);
AF_API void  af_strbuf_free(AfStrBuf* b);
AF_API void  af_strbuf_reserve(AfStrBuf* b, int32_t cap);
AF_API void  af_strbuf_clear(AfStrBuf* b);
AF_API void  af_strbuf_append(AfStrBuf* b, const char* s);
AF_API void  af_strbuf_append_n(AfStrBuf* b, const char* s, int32_t n);
AF_API void  af_strbuf_append_char(AfStrBuf* b, char c);
AF_API void  af_strbuf_append_str(AfStrBuf* b, AfStr s);
AF_API void  af_strbuf_appendf(AfStrBuf* b, const char* fmt, ...) AF_PRINTF(2, 3);
AF_API void  af_strbuf_vappendf(AfStrBuf* b, const char* fmt, va_list ap);
AF_API void  af_strbuf_insert(AfStrBuf* b, int32_t at, const char* s);
/** Erase [at, at+n). */
AF_API void  af_strbuf_erase(AfStrBuf* b, int32_t at, int32_t n);
/** Backing buffer as a view. Invalid after the next mutation. */
AF_INLINE AfStr af_strbuf_view(const AfStrBuf* b) { return af_str_n(b->data, b->len); }
AF_INLINE const char* af_strbuf_cstr(const AfStrBuf* b) {
    return b->data ? b->data : "";
}

/* ------------------------------------------------------------- case/util */
AF_API AfStr af_str_to_lower(AfStr s, char* scratch, int32_t scratch_size);
AF_API int   af_str_casecmp(AfStr a, const char* b);
AF_INLINE int af_char_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}
AF_INLINE int af_char_is_digit(char c) { return c >= '0' && c <= '9'; }
AF_INLINE int af_char_is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
AF_INLINE int af_char_is_alnum(char c) { return af_char_is_alpha(c) || af_char_is_digit(c); }
AF_INLINE char af_char_to_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}
/** True if `s` is a legal identifier (also accepts '.' for paths/names). */
AF_API int af_str_is_ident(AfStr s);
/** Safe identifier: replaces illegal bytes with '_'. */
AF_API AfStr af_str_sanitize_ident(AfStr s, char* scratch, int32_t scratch_size);
/** Formats a float compactly: 1.5 -> "1.5", 2.0 -> "2". */
AF_API AfStr af_str_format_f32(float v, char* scratch, int32_t scratch_size);

/* ----------------------------------------------------------------- hash */
typedef uint32_t AfHash;
/** FNV-1a. Stable across platforms -- used for cache keys and hashing. */
AF_INLINE AfHash af_hash_bytes(const void* data, size_t n) {
    const unsigned char* p = (const unsigned char*)data;
    AfHash h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= (AfHash)p[i];
        h *= 16777619u;
    }
    return h;
}
AF_INLINE AfHash af_hash_str(AfStr s) {
    return af_hash_bytes(s.data, (size_t)s.len);
}
AF_INLINE AfHash af_hash_cstr(const char* s) { return af_hash_str(af_str(s)); }
AF_INLINE AfHash af_hash_u32(uint32_t v) { return af_hash_bytes(&v, sizeof(v)); }
AF_INLINE AfHash af_hash_combine(AfHash a, AfHash b) {
    return af_hash_bytes(&b, sizeof(b)) ^ (a * 16777619u);
}

#endif /* AFNDLE_CORE_STR_H */
