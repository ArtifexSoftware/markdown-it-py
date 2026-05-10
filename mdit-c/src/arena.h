/*
 * arena.h — bump allocator with growable chunks.
 *
 * The arena is the only allocator parser code is allowed to use during
 * a parse. Every token, attribute, intermediate string, and helper
 * structure lives in the arena, which means lifetime tracking for those
 * objects collapses into a single ``mdit_arena_reset()`` call between
 * parses (cheap, O(num_chunks)).
 *
 * ``mdit_lib_ctx`` (allocators + OOM hook) is passed explicitly to every
 * entry point; it is not stored on the arena.
 *
 * Key properties:
 *   - Allocations are zero-initialised on demand via mdit_arena_zalloc().
 *   - Allocations are aligned to MDIT_ARENA_ALIGN bytes (16 by default,
 *     enough for any scalar in the project — we don't store SIMD data).
 *   - Chunks grow geometrically (×2) up to MDIT_ARENA_CHUNK_MAX.
 *   - mdit_arena_reset() keeps the largest chunk and frees the rest, so
 *     a hot parse loop reuses the buffer between documents at zero cost.
 *   - mdit_arena_destroy() releases everything.
 *   - Out-of-memory: ``ctx->oom`` is invoked; it must not return in
 *     normal use (tests may longjmp via a custom hook).
 */
#ifndef MDIT_SRC_ARENA_H
#define MDIT_SRC_ARENA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mdit/mdit_lib_ctx.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MDIT_ARENA_ALIGN
#  define MDIT_ARENA_ALIGN 16
#endif

#ifndef MDIT_ARENA_CHUNK_DEFAULT
#  define MDIT_ARENA_CHUNK_DEFAULT (16u * 1024u)  /* 16 KiB */
#endif

#ifndef MDIT_ARENA_CHUNK_MAX
#  define MDIT_ARENA_CHUNK_MAX (4u * 1024u * 1024u)  /* 4 MiB */
#endif

/* Forward declarations — the actual chunk layout is private. */
typedef struct mdit_arena_chunk mdit_arena_chunk;

typedef struct mdit_arena {
    mdit_arena_chunk *head;       /* most recent chunk (current cursor lives here) */
    size_t            next_chunk; /* preferred size for the next allocation */

    /* Diagnostics — cheap to keep, useful in tests. */
    size_t            total_used;     /* bytes handed out from arena_alloc */
    size_t            total_capacity; /* bytes mapped across all live chunks */
    size_t            num_chunks;
} mdit_arena;

/*
 * Initialise *a* in-place. ``initial_chunk_size`` is rounded up to a
 * sensible minimum; pass 0 for the default. The arena starts with no
 * chunks; the first allocation triggers the first chunk allocation.
 */
void  mdit_arena_init(mdit_arena *a, size_t initial_chunk_size);

/*
 * Free every chunk owned by the arena and zero its fields. Safe to call
 * on an arena that was init'd but never used.
 */
void  mdit_arena_destroy(mdit_lib_ctx *ctx, mdit_arena *a);

/*
 * Free all but the largest live chunk and rewind the cursor. The arena
 * is reusable immediately. Constant-time amortised, since steady-state
 * use settles at one chunk after the first parse.
 */
void  mdit_arena_reset(mdit_lib_ctx *ctx, mdit_arena *a);

/*
 * Hand out ``n`` bytes aligned to MDIT_ARENA_ALIGN. Returned pointer is
 * uninitialised. Invokes ``ctx->oom`` on allocation failure.
 * ``n == 0`` is well-defined and returns a non-NULL pointer to a unique
 * zero-byte slot (handy for sentinels).
 */
void *mdit_arena_alloc(mdit_lib_ctx *ctx, mdit_arena *a, size_t n);
void *mdit_arena_zalloc(mdit_lib_ctx *ctx, mdit_arena *a, size_t n);
void *mdit_arena_alloc_aligned(mdit_lib_ctx *ctx, mdit_arena *a, size_t n,
                               size_t alignment);

/*
 * Convenience: copy *n* bytes from ``data`` into a fresh arena slot.
 * Returns the new pointer.
 */
void *mdit_arena_dup(mdit_lib_ctx *ctx, mdit_arena *a, const void *data,
                     size_t n);

/*
 * Convenience: copy a NUL-terminated string. The result is also NUL-
 * terminated. ``s == NULL`` returns NULL.
 */
char *mdit_arena_strdup(mdit_lib_ctx *ctx, mdit_arena *a, const char *s);

/*
 * Try to grow the most recent allocation in place. Returns ``true`` on
 * success and updates ``*ptr`` if the live cursor was rewound to fit
 * the new size; returns ``false`` if the previous allocation was not
 * the most recent one or the chunk has insufficient room. Useful for
 * dynamic vectors that want to avoid copying when growing in-place.
 */
bool  mdit_arena_try_extend(mdit_lib_ctx *ctx, mdit_arena *a, void **ptr,
                            size_t old_size, size_t new_size);

#ifdef __cplusplus
}
#endif

#endif /* MDIT_SRC_ARENA_H */
