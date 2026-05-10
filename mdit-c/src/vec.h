/*
 * vec.h — typed dynamic arrays.
 *
 * Two flavours of growth:
 *   - Arena-backed (cheap, no per-element free): allocations come from
 *     an mdit_arena. Growing copies into a fresh arena slot when the
 *     vector outgrows its current bump-allocated buffer (mdit_arena
 *     supports try-extend so most growths are in-place).
 *   - malloc-backed: growth uses realloc(). Used at the API boundary
 *     where the caller wants ownership independent of any arena.
 *
 * The macros expand into thin static-inline functions so the type is
 * preserved and the optimiser can collapse the indirection. The header
 * pattern follows stb-style C: declare with MDIT_VEC_DECLARE() in a
 * header, expand with MDIT_VEC_DEFINE() in exactly one .c file.
 *
 * Naming: MDIT_VEC_DECLARE(name, T) generates `mdit_vec_<name>` plus
 * `mdit_vec_<name>_init`, `_push`, `_pop`, `_at`, `_clear`, `_reserve`
 * and friends.
 */
#ifndef MDIT_SRC_VEC_H
#define MDIT_SRC_VEC_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "mdit/mdit_lib_ctx.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Growth policy: 1.5x is a decent sweet spot — Facebook's
 * folly::fbvector uses it; CPython's list uses something close. It
 * gives geometric growth without doubling memory usage on near-power
 * boundaries (which matters for the arena, where wasted bytes pile up
 * until the next reset).
 */
static inline size_t mdit_vec_grow(size_t cap, size_t at_least)
{
    size_t want = cap < 4 ? 4 : cap + (cap >> 1);
    if (want < at_least) want = at_least;
    return want;
}

/* ---------------------------------------------------------------------
 * Generic shim: typed wrappers reuse a single byte-oriented core.
 * ------------------------------------------------------------------- */
typedef struct mdit_vec_core {
    void            *data;
    size_t           len;
    size_t           cap;
    mdit_lib_ctx    *lib;    /* required when arena != NULL */
    mdit_arena      *arena;   /* NULL = malloc-backed */
    size_t           elem;    /* element size, in bytes */
} mdit_vec_core;

void *mdit_vec_core_reserve(mdit_vec_core *v, size_t want_cap);
void *mdit_vec_core_push   (mdit_vec_core *v, const void *src);
void  mdit_vec_core_pop    (mdit_vec_core *v);
void  mdit_vec_core_clear  (mdit_vec_core *v);
void  mdit_vec_core_destroy(mdit_vec_core *v);

/* ---------------------------------------------------------------------
 * Typed wrapper macros
 *
 *   MDIT_VEC_DECLARE(name, TYPE) -- in headers
 *   MDIT_VEC_DEFINE(name, TYPE)  -- exactly once, in .c
 * ------------------------------------------------------------------- */
#define MDIT_VEC_DECLARE(NAME, TYPE)                                       \
    typedef struct mdit_vec_##NAME {                                       \
        TYPE           *data;                                              \
        size_t          len;                                               \
        size_t          cap;                                               \
        mdit_lib_ctx   *lib;                                               \
        mdit_arena     *arena;                                             \
        size_t          elem;                                              \
    } mdit_vec_##NAME;                                                     \
                                                                           \
    void  mdit_vec_##NAME##_init        (mdit_vec_##NAME *v,               \
                                         mdit_lib_ctx *lib,                \
                                         mdit_arena *arena);               \
    void  mdit_vec_##NAME##_destroy     (mdit_vec_##NAME *v);              \
    void  mdit_vec_##NAME##_clear       (mdit_vec_##NAME *v);              \
    bool  mdit_vec_##NAME##_reserve     (mdit_vec_##NAME *v, size_t cap);  \
    TYPE *mdit_vec_##NAME##_push        (mdit_vec_##NAME *v,               \
                                         TYPE value);                      \
    TYPE *mdit_vec_##NAME##_emplace     (mdit_vec_##NAME *v);              \
    void  mdit_vec_##NAME##_pop         (mdit_vec_##NAME *v);              \
    TYPE *mdit_vec_##NAME##_at          (mdit_vec_##NAME *v, size_t i);    \
    const TYPE *mdit_vec_##NAME##_at_c  (const mdit_vec_##NAME *v,         \
                                         size_t i);                        \
    size_t mdit_vec_##NAME##_len        (const mdit_vec_##NAME *v);        \
    /* end of MDIT_VEC_DECLARE */

#define MDIT_VEC_DEFINE(NAME, TYPE)                                        \
    void mdit_vec_##NAME##_init(mdit_vec_##NAME *v, mdit_lib_ctx *lib,     \
                                mdit_arena *arena)                         \
    {                                                                      \
        v->data  = NULL;                                                   \
        v->len   = 0;                                                      \
        v->cap   = 0;                                                      \
        v->lib   = lib;                                                    \
        v->arena = arena;                                                  \
        v->elem  = sizeof(TYPE);                                           \
    }                                                                      \
                                                                           \
    void mdit_vec_##NAME##_destroy(mdit_vec_##NAME *v)                     \
    {                                                                      \
        mdit_vec_core core;                                                \
        core.data  = v->data;                                              \
        core.len   = v->len;                                               \
        core.cap   = v->cap;                                               \
        core.lib   = v->lib;                                               \
        core.arena = v->arena;                                             \
        core.elem  = sizeof(TYPE);                                         \
        mdit_vec_core_destroy(&core);                                      \
        v->data = NULL; v->len = 0; v->cap = 0;                            \
    }                                                                      \
                                                                           \
    void mdit_vec_##NAME##_clear(mdit_vec_##NAME *v)                       \
    {                                                                      \
        v->len = 0;                                                        \
    }                                                                      \
                                                                           \
    bool mdit_vec_##NAME##_reserve(mdit_vec_##NAME *v, size_t cap)         \
    {                                                                      \
        if (cap <= v->cap) return true;                                    \
        mdit_vec_core core;                                                \
        core.data  = v->data;                                              \
        core.len   = v->len;                                               \
        core.cap   = v->cap;                                               \
        core.lib   = v->lib;                                               \
        core.arena = v->arena;                                             \
        core.elem  = sizeof(TYPE);                                         \
        void *r = mdit_vec_core_reserve(&core, cap);                       \
        if (r == NULL) return false;                                       \
        v->data = (TYPE *)r;                                               \
        v->cap  = core.cap;                                                \
        return true;                                                       \
    }                                                                      \
                                                                           \
    TYPE *mdit_vec_##NAME##_emplace(mdit_vec_##NAME *v)                    \
    {                                                                      \
        if (v->len == v->cap) {                                            \
            if (!mdit_vec_##NAME##_reserve(v,                              \
                    mdit_vec_grow(v->cap, v->len + 1))) {                  \
                return NULL;                                               \
            }                                                              \
        }                                                                  \
        TYPE *slot = &v->data[v->len++];                                   \
        return slot;                                                       \
    }                                                                      \
                                                                           \
    TYPE *mdit_vec_##NAME##_push(mdit_vec_##NAME *v, TYPE value)           \
    {                                                                      \
        TYPE *slot = mdit_vec_##NAME##_emplace(v);                         \
        if (slot != NULL) *slot = value;                                   \
        return slot;                                                       \
    }                                                                      \
                                                                           \
    void mdit_vec_##NAME##_pop(mdit_vec_##NAME *v)                         \
    {                                                                      \
        assert(v->len > 0);                                                \
        --v->len;                                                          \
    }                                                                      \
                                                                           \
    TYPE *mdit_vec_##NAME##_at(mdit_vec_##NAME *v, size_t i)               \
    {                                                                      \
        assert(i < v->len);                                                \
        return &v->data[i];                                                \
    }                                                                      \
                                                                           \
    const TYPE *mdit_vec_##NAME##_at_c(const mdit_vec_##NAME *v, size_t i) \
    {                                                                      \
        assert(i < v->len);                                                \
        return &v->data[i];                                                \
    }                                                                      \
                                                                           \
    size_t mdit_vec_##NAME##_len(const mdit_vec_##NAME *v)                 \
    {                                                                      \
        return v->len;                                                     \
    }                                                                      \
    /* end of MDIT_VEC_DEFINE */

#ifdef __cplusplus
}
#endif

#endif /* MDIT_SRC_VEC_H */
