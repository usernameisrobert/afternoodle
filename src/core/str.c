#include "afndle/core/str.h"

#include "afndle/core/math.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ============================================================== AfStr ops */

AfStr af_str_trim(AfStr s) {
    int32_t start = 0, end = s.len;
    while (start < end && af_char_is_space(s.data[start])) start++;
    while (end > start && af_char_is_space(s.data[end - 1])) end--;
    return af_str_slice(s, start, end);
}

AfStr af_str_to_lower(AfStr s, char *scratch, int32_t scratch_size) {
    af_str_cpy_max(s, scratch, scratch_size);
    for (int32_t i = 0; scratch[i]; i++) scratch[i] = af_char_to_lower(scratch[i]);
    return af_str(scratch);
}

int af_str_casecmp(AfStr a, const char *b) {
    if (!b) return a.len ? 1 : 0;
    int32_t n = (int32_t)strlen(b);
    int32_t m = a.len < n ? a.len : n;
    for (int32_t i = 0; i < m; i++) {
        char ca = af_char_to_lower(a.data[i]);
        char cb = af_char_to_lower(b[i]);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (a.len == n) return 0;
    return a.len < n ? -1 : 1;
}

int af_str_is_ident(AfStr s) {
    if (s.len <= 0) return 0;
    if (!af_char_is_alpha(s.data[0])) return 0;
    for (int32_t i = 1; i < s.len; i++)
        if (!af_char_is_alnum(s.data[i])) return 0;
    return 1;
}

AfStr af_str_sanitize_ident(AfStr s, char *scratch, int32_t scratch_size) {
    af_str_cpy_max(s, scratch, scratch_size);
    for (int32_t i = 0; scratch[i]; i++) {
        char c = scratch[i];
        if (i == 0) {
            if (!af_char_is_alpha(c)) scratch[i] = '_';
        } else if (!af_char_is_alnum(c)) {
            scratch[i] = '_';
        }
    }
    return af_str(scratch);
}

AfStr af_str_format_f32(float v, char *scratch, int32_t scratch_size) {
    if (v == (float)(int)v && af_absf(v) < 1e7f) {
        snprintf(scratch, (size_t)scratch_size, "%d", (int)v);
    } else {
        snprintf(scratch, (size_t)scratch_size, "%.4g", (double)v);
    }
    return af_str(scratch);
}

/* =========================================================== AfStrBuf impl */

void af_strbuf_init(AfStrBuf *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void af_strbuf_free(AfStrBuf *b) {
    af_free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void af_strbuf_reserve(AfStrBuf *b, int32_t cap) {
    if (b->cap >= cap) return;
    int32_t new_cap = b->cap ? b->cap : 64;
    while (new_cap < cap) new_cap *= 2;
    b->data = (char *)af_realloc(b->data, (size_t)new_cap);
    b->cap = new_cap;
    if (b->len == 0) b->data[0] = '\0';
}

void af_strbuf_clear(AfStrBuf *b) {
    b->len = 0;
    if (b->data) b->data[0] = '\0';
}

void af_strbuf_append_n(AfStrBuf *b, const char *s, int32_t n) {
    if (!s || n <= 0) return;
    af_strbuf_reserve(b, b->len + n + 1);
    memcpy(b->data + b->len, s, (size_t)n);
    b->len += n;
    b->data[b->len] = '\0';
}

void af_strbuf_append(AfStrBuf *b, const char *s) {
    if (!s) return;
    af_strbuf_append_n(b, s, (int32_t)strlen(s));
}

void af_strbuf_append_char(AfStrBuf *b, char c) { af_strbuf_append_n(b, &c, 1); }

void af_strbuf_append_str(AfStrBuf *b, AfStr s) {
    af_strbuf_append_n(b, s.data, s.len);
}

void af_strbuf_vappendf(AfStrBuf *b, const char *fmt, va_list ap) {
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n <= 0) return;
    af_strbuf_reserve(b, b->len + n + 1);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    b->len += n;
}

void af_strbuf_appendf(AfStrBuf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    af_strbuf_vappendf(b, fmt, ap);
    va_end(ap);
}

void af_strbuf_insert(AfStrBuf *b, int32_t at, const char *s) {
    if (!s) return;
    int32_t n = (int32_t)strlen(s);
    if (at < 0) at = 0;
    if (at > b->len) at = b->len;
    af_strbuf_reserve(b, b->len + n + 1);
    memmove(b->data + at + n, b->data + at, (size_t)(b->len - at));
    memcpy(b->data + at, s, (size_t)n);
    b->len += n;
    b->data[b->len] = '\0';
}

void af_strbuf_erase(AfStrBuf *b, int32_t at, int32_t n) {
    if (at < 0) { n += at; at = 0; }
    if (at >= b->len) return;
    if (at + n > b->len) n = b->len - at;
    memmove(b->data + at, b->data + at + n, (size_t)(b->len - at - n));
    b->len -= n;
    b->data[b->len] = '\0';
}
