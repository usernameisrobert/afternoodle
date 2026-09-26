#include "afndle/core/mem.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "afndle/core/log.h"
#include "afndle/core/time.h"

/* ============================================================== heap util */

void *af_malloc(size_t size) {
    if (size == 0) size = 1;
    void *p = malloc(size);
    if (!p) af_panic(__FILE__, __LINE__, "out of memory allocating %" AF_SIZE_FMT " bytes", AF_SIZE_ARG(size));
    return p;
}

void *af_calloc(size_t count, size_t size) {
    if (count == 0 || size == 0) { count = 1; size = 1; }
    void *p = calloc(count, size);
    if (!p) af_panic(__FILE__, __LINE__, "out of memory allocating %" AF_SIZE_FMT " x %" AF_SIZE_FMT, AF_SIZE_ARG(count), AF_SIZE_ARG(size));
    return p;
}

void *af_realloc(void *p, size_t size) {
    if (size == 0) size = 1;
    void *n = realloc(p, size);
    if (!n) af_panic(__FILE__, __LINE__, "out of memory reallocating %" AF_SIZE_FMT " bytes", AF_SIZE_ARG(size));
    return n;
}

void af_free(void *p) { free(p); }

void *af_aligned_alloc(size_t size, size_t align) {
    if (align < sizeof(void *)) align = sizeof(void *);
    /* align must be a power of two */
    if (align & (align - 1)) af_panic(__FILE__, __LINE__, "alignment %" AF_SIZE_FMT " is not a power of two", AF_SIZE_ARG(align));
    size_t rounded = (size + align - 1) & ~(align - 1);
    void *raw = malloc(rounded + align);
    if (!raw) af_panic(__FILE__, __LINE__, "out of memory allocating %" AF_SIZE_FMT " aligned bytes", AF_SIZE_ARG(size));
    uintptr_t base = (uintptr_t)raw + align;
    base &= ~(uintptr_t)(align - 1);
    ((void **)base)[-1] = raw; /* stash the malloc pointer just below */
    return (void *)base;
}

void af_aligned_free(void *p) {
    if (!p) return;
    free(((void **)p)[-1]);
}

char *af_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = (char *)af_malloc(n);
    memcpy(d, s, n);
    return d;
}

char *af_strndup(const char *s, size_t n) {
    if (!s) return NULL;
    size_t len = 0;
    while (len < n && s[len]) len++;
    char *d = (char *)af_malloc(len + 1);
    memcpy(d, s, len);
    d[len] = '\0';
    return d;
}

char *af_vsprintf(const char *fmt, va_list ap) {
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0) return af_strdup("");
    char *buf = (char *)af_malloc((size_t)n + 1);
    vsnprintf(buf, (size_t)n + 1, fmt, ap);
    return buf;
}

char *af_sprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *s = af_vsprintf(fmt, ap);
    va_end(ap);
    return s;
}

/* ======================================================== alloc accounting */

static AfMemStats g_mem_stats;

AfMemStats af_mem_stats(void) { return g_mem_stats; }
void       af_mem_stats_reset(void) { memset(&g_mem_stats, 0, sizeof(g_mem_stats)); }

/* ================================================================ arena */

struct AfArenaBlock {
    AfArenaBlock *next;
    size_t        size;
    size_t        used;
    /* data follows */
};

#define AF_ARENA_HEADER sizeof(AfArenaBlock)
#define AF_ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((size_t)(a) - 1))
#define AF_DEFAULT_ALIGN 16

static void* block_data(AfArenaBlock *b) { return (char *)b + AF_ARENA_HEADER; }

static AfArenaBlock *arena_new_block(size_t size) {
    if (size < AF_ARENA_DEFAULT_BLOCK) size = AF_ARENA_DEFAULT_BLOCK;
    AfArenaBlock *b = (AfArenaBlock *)calloc(1, size);
    if (!b) af_panic(__FILE__, __LINE__, "arena: out of memory (%" AF_SIZE_FMT " byte block)", AF_SIZE_ARG(size));
    b->size = size - AF_ARENA_HEADER;
    b->used = 0;
    return b;
}

AfArena *af_arena_create(size_t block_size) {
    AfArena *a = (AfArena *)af_calloc(1, sizeof(AfArena));
    a->block_size = block_size ? block_size : AF_ARENA_DEFAULT_BLOCK;
    return a;
}

AfArena *af_arena_create_static(void *mem, size_t size) {
    /* One big block, no malloc. Growing is not supported. */
    if (size < AF_ARENA_HEADER + 64) return NULL;
    AfArenaBlock *b = (AfArenaBlock *)mem;
    memset(b, 0, AF_ARENA_HEADER);
    b->size = size - AF_ARENA_HEADER;
    b->used = 0;
    b->next = NULL;
    AfArena *a = (AfArena *)af_calloc(1, sizeof(AfArena));
    a->head = b;
    a->block_size = size;
    a->total = size;
    return a;
}

void af_arena_destroy(AfArena *a) {
    if (!a) return;
    /* Only free blocks we malloc'd. A static arena has a single block whose
     * memory we do not own; detect it by checking for a non-heap-looking
     * block (we set block_size == total for static arenas). */
    int is_static = (a->block_size == a->total && a->head && a->head->next == NULL);
    if (!is_static) {
        AfArenaBlock *b = a->head;
        while (b) {
            AfArenaBlock *next = b->next;
            free(b);
            b = next;
        }
    }
    free(a);
}

void af_arena_reset(AfArena *a) {
    if (!a) return;
    AfArenaBlock *b = a->head;
    while (b) {
        b->used = 0;
        b = b->next;
    }
    a->used = 0;
}

void *af_arena_alloc_aligned(AfArena *a, size_t size, size_t align) {
    if (!a) return NULL;
    if (align == 0) align = AF_DEFAULT_ALIGN;
    if (size == 0) size = 1;

    /* Fast path: fits in the head block. */
    if (a->head) {
        size_t base = (size_t)block_data(a->head);
        size_t cur = base + a->head->used;
        size_t aligned = AF_ALIGN_UP(cur, align);
        size_t pad = aligned - cur;
        if (aligned + size <= a->head->size) {
            /* Keep the header-aligned layout so reset is a memset. */
            if (pad + size <= a->head->size - a->head->used) {
                a->head->used += pad + size;
                a->used = a->head->used;
                return (void *)aligned;
            }
        }
    }

    /* Need a new block, large enough for alignment slack. */
    size_t need = size + align;
    if (need < a->block_size) need = a->block_size;
    AfArenaBlock *nb = arena_new_block(need);
    nb->next = a->head;
    a->head = nb;
    a->total += nb->size;

    size_t base = (size_t)block_data(nb);
    size_t aligned = AF_ALIGN_UP(base, align);
    size_t pad = aligned - base;
    nb->used = pad + size;
    a->used = nb->used;
    return (void *)aligned;
}

void *af_arena_alloc(AfArena *a, size_t size) {
    return af_arena_alloc_aligned(a, size, AF_DEFAULT_ALIGN);
}

void *af_arena_calloc(AfArena *a, size_t count, size_t size) {
    size_t total = count * size;
    if (count != 0 && total / count != size)
        af_panic(__FILE__, __LINE__, "arena: overflow in calloc(%" AF_SIZE_FMT ", %" AF_SIZE_FMT ")", AF_SIZE_ARG(count), AF_SIZE_ARG(size));
    void *p = af_arena_alloc(a, total);
    memset(p, 0, total);
    return p;
}

char *af_arena_strdup(AfArena *a, const char *s) {
    if (!s) return NULL;
    return af_arena_strndup(a, s, strlen(s));
}

char *af_arena_strndup(AfArena *a, const char *s, size_t n) {
    if (!s) return NULL;
    size_t len = 0;
    while (len < n && s[len]) len++;
    char *d = (char *)af_arena_alloc(a, len + 1);
    memcpy(d, s, len);
    d[len] = '\0';
    return d;
}

char *af_arena_printf(AfArena *a, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return af_arena_strdup(a, "");
    char *buf = (char *)af_arena_alloc(a, (size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(buf, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return buf;
}

char *af_arena_append(AfArena *a, const char *src, size_t n, char **buf, size_t *cap) {
    if (n == 0) return *buf;
    if (*buf == NULL || *cap < n + 1) {
        size_t new_cap = *cap ? *cap * 2 : 256;
        while (new_cap < n + 1) new_cap *= 2;
        *buf = (char *)af_arena_alloc(a, new_cap);
        *cap = new_cap;
        if (*cap > n + 1) (*buf)[0] = '\0';
    }
    memcpy(*buf, src, n);
    (*buf)[n] = '\0';
    return *buf;
}
