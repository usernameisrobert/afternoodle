/* Arena (linear) allocator + scratch buffers. Fast, no free, bump pointer. */
#ifndef AFNDLE_CORE_MEM_H
#define AFNDLE_CORE_MEM_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include "afndle/core/afconfig.h"

/* Arena: allocate by bumping, free everything at once. The workhorse for
 * per-frame and per-command scratch. Not thread safe; give each thread one. */
typedef struct AfArenaBlock AfArenaBlock;

typedef struct {
    AfArenaBlock* head;
    size_t total;    /* bytes reserved across all blocks */
    size_t used;     /* bytes handed out in the head block */
    size_t block_size;
} AfArena;

#define AF_ARENA_DEFAULT_BLOCK (64u * 1024u)

/* If `out` is non-zero, alloc uses it instead of malloc (static pool). */
AF_API AfArena* af_arena_create(size_t block_size);
AF_API AfArena* af_arena_create_static(void* mem, size_t size);
AF_API void     af_arena_destroy(AfArena* a);
AF_API void     af_arena_reset(AfArena* a);
AF_API void*    af_arena_alloc(AfArena* a, size_t size);
AF_API void*    af_arena_alloc_aligned(AfArena* a, size_t size, size_t align);
AF_API void*    af_arena_calloc(AfArena* a, size_t count, size_t size);
char*           af_arena_strdup(AfArena* a, const char* s);
char*           af_arena_strndup(AfArena* a, const char* s, size_t n);
char*           af_arena_printf(AfArena* a, const char* fmt, ...) AF_PRINTF(2, 3);
/** Copy `src` into an arena buffer, grow-on-demand. `*cap` is in bytes. */
char*           af_arena_append(AfArena* a, const char* src, size_t n, char** buf,
                                size_t* cap);
#define AF_ARENA_STR(a, b, c) af_arena_append((a), (b), (c), &(b), &(c))

/* -------------------------------------------------------------- heap util */
AF_API void* af_malloc(size_t size);
AF_API void* af_calloc(size_t count, size_t size);
AF_API void* af_realloc(void* p, size_t size);
AF_API void  af_free(void* p);
/** Aligned alloc for SIMD-friendly data (e.g. 16-byte vertex buffers). */
AF_API void* af_aligned_alloc(size_t size, size_t align);
AF_API void  af_aligned_free(void* p);
/** Duplicate with the platform allocator (not the arena). Caller frees. */
char*        af_strdup(const char* s);
AF_API char* af_strndup(const char* s, size_t n);
AF_API char* af_vsprintf(const char* fmt, va_list ap);
AF_API char* af_sprintf(const char* fmt, ...) AF_PRINTF(1, 2);

/* ------------------------------------------------------- allocation stats */
typedef struct {
    size_t live_bytes;
    size_t peak_bytes;
    uint64_t total_allocs;
    uint64_t total_frees;
} AfMemStats;
AF_API AfMemStats af_mem_stats(void);
AF_API void       af_mem_stats_reset(void);

#endif /* AFNDLE_CORE_MEM_H */
