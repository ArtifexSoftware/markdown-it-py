/*
 * arena.c — bump allocator implementation.
 *
 * Layout: each chunk is a single malloc, with a header followed by the
 * payload. The arena keeps a singly-linked list with the *most recent*
 * chunk at ``head``; older chunks are kept around so existing pointers
 * stay valid until reset/destroy.
 */
#include "arena.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#  define MDIT_INLINE __inline
#else
#  define MDIT_INLINE inline
#endif

struct mdit_arena_chunk {
    mdit_arena_chunk *next;     /* older chunks down the list */
    size_t            cap;      /* total payload capacity in bytes */
    size_t            used;     /* bytes consumed from the payload */
    /* The payload begins immediately after this struct, suitably
     * aligned (we pad capacity below to honour MDIT_ARENA_ALIGN). */
};

/* ---------------------------------------------------------------------
 * OOM handling
 * ------------------------------------------------------------------- */
static void default_oom_handler(size_t requested)
{
    (void)fprintf(stderr,
                  "mdit-c: arena allocation failed (requested %zu bytes)\n",
                  requested);
    abort();
}

static mdit_arena_oom_fn s_oom_handler = default_oom_handler;

void mdit_arena_set_oom_handler(mdit_arena_oom_fn fn)
{
    s_oom_handler = fn ? fn : default_oom_handler;
}

static void oom(size_t requested)
{
    s_oom_handler(requested);
    /* If a test handler returns, fall through to abort to avoid UB. */
    abort();
}

/* ---------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------- */
static MDIT_INLINE size_t round_up_align(size_t x, size_t a)
{
    /* a is a power of two by contract; verified via assert at use sites. */
    return (x + (a - 1)) & ~(a - 1);
}

static MDIT_INLINE bool is_pow2(size_t x)
{
    return x != 0 && (x & (x - 1)) == 0;
}

static MDIT_INLINE size_t header_size(void)
{
    /* Pad the chunk header up to MDIT_ARENA_ALIGN so the payload starts
     * aligned without per-allocation fixup. */
    return round_up_align(sizeof(mdit_arena_chunk), MDIT_ARENA_ALIGN);
}

static MDIT_INLINE unsigned char *chunk_payload(mdit_arena_chunk *c)
{
    return (unsigned char *)c + header_size();
}

static mdit_arena_chunk *new_chunk(size_t payload_cap)
{
    /* Always allocate at least one cache line of payload. */
    if (payload_cap < 64) payload_cap = 64;
    payload_cap = round_up_align(payload_cap, MDIT_ARENA_ALIGN);

    size_t total = header_size() + payload_cap;
    /* Overflow check: header_size() is a small constant, so any overflow
     * here means payload_cap was already absurd. */
    if (total < payload_cap) {
        oom(payload_cap);
    }

    mdit_arena_chunk *c = (mdit_arena_chunk *)malloc(total);
    if (c == NULL) {
        oom(total);
    }
    c->next = NULL;
    c->cap  = payload_cap;
    c->used = 0;
    return c;
}

static size_t pick_chunk_size(size_t requested_payload, size_t hint)
{
    /* Caller wants at least ``requested_payload`` bytes. ``hint`` is the
     * arena's preferred next-chunk size; we honour it as-is so callers
     * (incl. tests) can opt into small chunks. We only fall back to the
     * default when no hint is set, and we always clamp to the maximum. */
    size_t want = hint != 0 ? hint : MDIT_ARENA_CHUNK_DEFAULT;
    if (want > MDIT_ARENA_CHUNK_MAX) {
        want = MDIT_ARENA_CHUNK_MAX;
    }
    if (want < requested_payload) {
        want = requested_payload;
    }
    return want;
}

static MDIT_INLINE size_t chunk_remaining(const mdit_arena_chunk *c)
{
    return c->cap - c->used;
}

/* ---------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------- */
void mdit_arena_init(mdit_arena *a, size_t initial_chunk_size)
{
    assert(a != NULL);
    a->head           = NULL;
    a->next_chunk     = initial_chunk_size != 0
                            ? initial_chunk_size
                            : MDIT_ARENA_CHUNK_DEFAULT;
    a->total_used     = 0;
    a->total_capacity = 0;
    a->num_chunks     = 0;
}

void mdit_arena_destroy(mdit_arena *a)
{
    if (a == NULL) return;
    mdit_arena_chunk *c = a->head;
    while (c != NULL) {
        mdit_arena_chunk *next = c->next;
        free(c);
        c = next;
    }
    a->head           = NULL;
    a->next_chunk     = 0;
    a->total_used     = 0;
    a->total_capacity = 0;
    a->num_chunks     = 0;
}

void mdit_arena_reset(mdit_arena *a)
{
    if (a == NULL || a->head == NULL) {
        if (a != NULL) {
            a->total_used = 0;
        }
        return;
    }

    /* Find the largest chunk; keep it, drop the rest. */
    mdit_arena_chunk *biggest = a->head;
    for (mdit_arena_chunk *c = a->head; c != NULL; c = c->next) {
        if (c->cap > biggest->cap) {
            biggest = c;
        }
    }
    mdit_arena_chunk *c = a->head;
    size_t kept_cap = biggest->cap;
    while (c != NULL) {
        mdit_arena_chunk *next = c->next;
        if (c != biggest) {
            free(c);
        }
        c = next;
    }
    biggest->next   = NULL;
    biggest->used   = 0;
    a->head         = biggest;
    a->total_used   = 0;
    a->total_capacity = kept_cap;
    a->num_chunks   = 1;
    /* Keep the growth hint so the next big parse doesn't shrink. */
    if (a->next_chunk < kept_cap) {
        a->next_chunk = kept_cap;
    }
}

/* ---------------------------------------------------------------------
 * Allocation
 * ------------------------------------------------------------------- */
static void *raw_alloc(mdit_arena *a, size_t n, size_t alignment)
{
    assert(a != NULL);
    assert(is_pow2(alignment));

    /* Bump zero-byte requests to one byte so adjacent zero-sized
     * allocations get distinct pointers. We still only count the
     * caller-requested ``n`` against ``total_used`` for diagnostics. */
    size_t alloc_n = n == 0 ? 1 : n;

    /* Honour the requested alignment in the chunk-local cursor by
     * walking ``used`` up to the next aligned offset. The chunk
     * payload is already aligned to MDIT_ARENA_ALIGN so anything
     * <= MDIT_ARENA_ALIGN is free. */
    if (a->head == NULL) {
        size_t want = pick_chunk_size(alloc_n, a->next_chunk);
        a->head = new_chunk(want);
        a->total_capacity += a->head->cap;
        a->num_chunks     += 1;
        if (a->next_chunk < MDIT_ARENA_CHUNK_MAX) {
            a->next_chunk = (a->next_chunk < MDIT_ARENA_CHUNK_MAX / 2)
                                ? a->next_chunk * 2
                                : MDIT_ARENA_CHUNK_MAX;
        }
    }

    mdit_arena_chunk *c = a->head;
    /* Align cursor for *this* allocation. */
    size_t aligned_used = round_up_align(c->used, alignment);
    if (aligned_used > c->cap || c->cap - aligned_used < alloc_n) {
        /* Fresh chunk needed. */
        size_t want = pick_chunk_size(alloc_n, a->next_chunk);
        mdit_arena_chunk *fresh = new_chunk(want);
        fresh->next = c;
        a->head = fresh;
        a->total_capacity += fresh->cap;
        a->num_chunks     += 1;
        if (a->next_chunk < MDIT_ARENA_CHUNK_MAX) {
            a->next_chunk = (a->next_chunk < MDIT_ARENA_CHUNK_MAX / 2)
                                ? a->next_chunk * 2
                                : MDIT_ARENA_CHUNK_MAX;
        }
        c = fresh;
        aligned_used = round_up_align(c->used, alignment);
        /* Brand new chunk: cursor is 0, already aligned. */
    }

    void *out = chunk_payload(c) + aligned_used;
    c->used = aligned_used + alloc_n;
    a->total_used += n;
    return out;
}

void *mdit_arena_alloc(mdit_arena *a, size_t n)
{
    return raw_alloc(a, n, MDIT_ARENA_ALIGN);
}

void *mdit_arena_zalloc(mdit_arena *a, size_t n)
{
    void *p = raw_alloc(a, n, MDIT_ARENA_ALIGN);
    if (n != 0) {
        memset(p, 0, n);
    }
    return p;
}

void *mdit_arena_alloc_aligned(mdit_arena *a, size_t n, size_t alignment)
{
    if (alignment == 0) alignment = MDIT_ARENA_ALIGN;
    if (!is_pow2(alignment)) {
        oom(n); /* programming error; treat as fatal in debug builds */
    }
    if (alignment < MDIT_ARENA_ALIGN) alignment = MDIT_ARENA_ALIGN;
    return raw_alloc(a, n, alignment);
}

void *mdit_arena_dup(mdit_arena *a, const void *data, size_t n)
{
    void *p = mdit_arena_alloc(a, n);
    if (n != 0 && data != NULL) {
        memcpy(p, data, n);
    }
    return p;
}

char *mdit_arena_strdup(mdit_arena *a, const char *s)
{
    if (s == NULL) return NULL;
    size_t len = strlen(s);
    char *out  = (char *)mdit_arena_alloc(a, len + 1);
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

bool mdit_arena_try_extend(mdit_arena *a, void **ptr,
                           size_t old_size, size_t new_size)
{
    assert(a != NULL && ptr != NULL);
    if (a->head == NULL || *ptr == NULL) return false;
    if (new_size <= old_size) {
        /* Trivially fits; just lie and report success without moving. */
        return true;
    }

    mdit_arena_chunk *c = a->head;
    unsigned char    *payload = chunk_payload(c);
    unsigned char    *cursor  = payload + c->used;
    unsigned char    *prev_end = ((unsigned char *)*ptr) + old_size;

    /* The allocation can only be extended in place if it sits at the
     * very tail of the live chunk (i.e. nothing has been allocated
     * after it). */
    if (prev_end != cursor) return false;

    size_t extra = new_size - old_size;
    if (chunk_remaining(c) < extra) return false;

    c->used       += extra;
    a->total_used += extra;
    return true;
}
