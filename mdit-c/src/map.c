/*
 * map.c — small ordered string→value map.
 *
 * Linear search by design: typical N <= 8 (token attrs, options).
 * If profiling ever shows this is hot we'll move to robin-hood
 * hashing, but the straight loop wins for the sizes we care about
 * and matches the upstream behaviour of preserving insertion order.
 */
#include "map.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------
 * Internal growth — mirrors the policy in vec.c but with the map's
 * own entry type. We keep a private copy rather than depending on
 * the typed vec macros so map's lifetime story stays single-file.
 * ------------------------------------------------------------------- */
static bool ensure_cap(mdit_map *m, size_t want_cap)
{
    if (want_cap <= m->cap) return true;

    size_t cap = m->cap < 4 ? 4 : m->cap + (m->cap >> 1);
    if (cap < want_cap) cap = want_cap;

    size_t bytes = cap * sizeof(mdit_map_entry);
    if (cap > (size_t)-1 / sizeof(mdit_map_entry)) return false;

    if (m->arena != NULL) {
        mdit_map_entry *fresh = (mdit_map_entry *)mdit_arena_alloc(m->arena, bytes);
        if (m->len > 0) {
            memcpy(fresh, m->data, m->len * sizeof(mdit_map_entry));
        }
        m->data = fresh;
        m->cap  = cap;
        return true;
    }

    mdit_map_entry *fresh = (mdit_map_entry *)realloc(m->data, bytes);
    if (fresh == NULL) return false;
    m->data = fresh;
    m->cap  = cap;
    return true;
}

static size_t find_index(const mdit_map *m, mdit_str key)
{
    for (size_t i = 0; i < m->len; ++i) {
        if (mdit_str_eq(m->data[i].key, key)) return i;
    }
    return (size_t)-1;
}

/* ---------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------- */
void mdit_map_init(mdit_map *m, mdit_arena *arena)
{
    m->data  = NULL;
    m->len   = 0;
    m->cap   = 0;
    m->arena = arena;
}

void mdit_map_destroy(mdit_map *m)
{
    if (m == NULL) return;
    if (m->arena == NULL && m->data != NULL) {
        free(m->data);
    }
    m->data = NULL;
    m->len  = 0;
    m->cap  = 0;
}

void mdit_map_clear(mdit_map *m)
{
    m->len = 0;
}

size_t mdit_map_len(const mdit_map *m)
{
    return m->len;
}

const mdit_map_entry *mdit_map_at(const mdit_map *m, size_t i)
{
    if (i >= m->len) return NULL;
    return &m->data[i];
}

/* ---------------------------------------------------------------------
 * Lookup
 * ------------------------------------------------------------------- */
bool mdit_map_has(const mdit_map *m, mdit_str key)
{
    return find_index(m, key) != (size_t)-1;
}

const mdit_value *mdit_map_get(const mdit_map *m, mdit_str key)
{
    size_t idx = find_index(m, key);
    if (idx == (size_t)-1) return NULL;
    return &m->data[idx].value;
}

const mdit_value *mdit_map_get_z(const mdit_map *m, const char *key)
{
    if (key == NULL) return NULL;
    mdit_str k; k.data = key; k.len = strlen(key);
    return mdit_map_get(m, k);
}

/* ---------------------------------------------------------------------
 * Mutation
 * ------------------------------------------------------------------- */
bool mdit_map_set(mdit_map *m, mdit_str key, mdit_value value)
{
    size_t idx = find_index(m, key);
    if (idx != (size_t)-1) {
        m->data[idx].value = value;
        return true;
    }
    if (m->len == m->cap && !ensure_cap(m, m->len + 1)) return false;
    m->data[m->len].key   = key;
    m->data[m->len].value = value;
    ++m->len;
    return true;
}

bool mdit_map_set_z(mdit_map *m, const char *key, mdit_value value)
{
    if (key == NULL) return false;
    mdit_str k; k.data = key; k.len = strlen(key);
    return mdit_map_set(m, k, value);
}

bool mdit_map_del(mdit_map *m, mdit_str key)
{
    size_t idx = find_index(m, key);
    if (idx == (size_t)-1) return false;
    /* Shift the tail left so insertion order of remaining entries
     * is preserved. memmove handles overlapping regions correctly. */
    size_t tail = m->len - idx - 1;
    if (tail > 0) {
        memmove(&m->data[idx], &m->data[idx + 1],
                tail * sizeof(mdit_map_entry));
    }
    --m->len;
    return true;
}
