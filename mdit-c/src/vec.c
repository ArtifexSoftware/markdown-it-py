/*
 * vec.c — byte-oriented core that the typed wrappers in vec.h share.
 *
 * Keeping the core untyped means we don't pay for one .o per element
 * type and the growth policy lives in exactly one place.
 */
#include "vec.h"

#include <stdlib.h>
#include <string.h>

void *mdit_vec_core_reserve(mdit_vec_core *v, size_t want_cap)
{
    if (want_cap <= v->cap) return v->data;

    size_t bytes = want_cap * v->elem;
    /* Overflow check: if want_cap*elem wraps we must abort. */
    if (v->elem != 0 && want_cap > (size_t)-1 / v->elem) {
        return NULL;
    }

    if (v->arena != NULL) {
        if (v->data == NULL) {
            v->data = mdit_arena_alloc(v->arena, bytes);
            v->cap  = want_cap;
            return v->data;
        }
        /* Try to grow in place at the arena tail. */
        size_t old_bytes = v->cap * v->elem;
        void *p = v->data;
        if (mdit_arena_try_extend(v->arena, &p, old_bytes, bytes)) {
            v->data = p;
            v->cap  = want_cap;
            return v->data;
        }
        /* Otherwise, allocate a fresh slab and copy. The previous
         * allocation is leaked into the arena until reset(); that is
         * the trade we accept for not having a true free(). */
        void *fresh = mdit_arena_alloc(v->arena, bytes);
        if (v->len > 0) {
            memcpy(fresh, v->data, v->len * v->elem);
        }
        v->data = fresh;
        v->cap  = want_cap;
        return v->data;
    }

    /* malloc-backed. */
    void *fresh = realloc(v->data, bytes);
    if (fresh == NULL) return NULL;
    v->data = fresh;
    v->cap  = want_cap;
    return v->data;
}

void *mdit_vec_core_push(mdit_vec_core *v, const void *src)
{
    if (v->len == v->cap) {
        if (mdit_vec_core_reserve(v, mdit_vec_grow(v->cap, v->len + 1)) == NULL) {
            return NULL;
        }
    }
    void *slot = (char *)v->data + v->len * v->elem;
    if (src != NULL) {
        memcpy(slot, src, v->elem);
    }
    ++v->len;
    return slot;
}

void mdit_vec_core_pop(mdit_vec_core *v)
{
    if (v->len == 0) return;
    --v->len;
}

void mdit_vec_core_clear(mdit_vec_core *v)
{
    v->len = 0;
}

void mdit_vec_core_destroy(mdit_vec_core *v)
{
    if (v->arena == NULL && v->data != NULL) {
        free(v->data);
    }
    v->data = NULL;
    v->len  = 0;
    v->cap  = 0;
}
