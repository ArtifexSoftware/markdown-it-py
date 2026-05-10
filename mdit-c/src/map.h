/*
 * map.h — small ordered string→value map.
 *
 * Used for token attributes, parser options, render env, and Token.meta.
 * Three properties drive the design:
 *
 *   1. Insertion order matters. Upstream `markdown-it-py` serializes
 *      `Token.attrs` as a list of `[k, v]` pairs (`as_upstream=True`)
 *      and therefore preserves order. The C port has to match.
 *
 *   2. N is small. Typical attrs hold 0–3 entries (`href`, `title`,
 *      `class`); `options` holds a couple of dozen at most. A linear
 *      vector of pairs beats a hash table for these sizes (and is
 *      friendlier to the cache).
 *
 *   3. Values are heterogeneous. Token attrs accept str | int | float
 *      in Python; the C side mirrors that with a tagged union.
 *
 * The map stores keys as borrowed string views into the arena (callers
 * are responsible for ensuring the key memory outlives the map — in
 * practice, all keys are interned string literals or arena-owned).
 *
 * Two flavours of value are supported:
 *
 *   - mdit_value: tagged union { string | int64 | double | bool | null }
 *     used for `attrs`, `options`, `meta`.
 *   - For env we'll add a typed wrapper later; the value layer is
 *     enough for Phase 1.
 */
#ifndef MDIT_SRC_MAP_H
#define MDIT_SRC_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "mdit/mdit_lib_ctx.h"
#include "str.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mdit_value_kind {
    MDIT_VALUE_NULL = 0,
    MDIT_VALUE_BOOL,
    MDIT_VALUE_INT,
    MDIT_VALUE_DOUBLE,
    MDIT_VALUE_STR
} mdit_value_kind;

typedef struct mdit_value {
    mdit_value_kind kind;
    union {
        bool        b;
        int64_t     i;
        double      d;
        mdit_str    s;   /* borrowed */
    } u;
} mdit_value;

static inline mdit_value mdit_value_null(void)
{
    mdit_value v; v.kind = MDIT_VALUE_NULL; v.u.i = 0; return v;
}
static inline mdit_value mdit_value_bool(bool b)
{
    mdit_value v; v.kind = MDIT_VALUE_BOOL; v.u.b = b; return v;
}
static inline mdit_value mdit_value_int(int64_t i)
{
    mdit_value v; v.kind = MDIT_VALUE_INT; v.u.i = i; return v;
}
static inline mdit_value mdit_value_double(double d)
{
    mdit_value v; v.kind = MDIT_VALUE_DOUBLE; v.u.d = d; return v;
}
static inline mdit_value mdit_value_str(mdit_str s)
{
    mdit_value v; v.kind = MDIT_VALUE_STR; v.u.s = s; return v;
}
static inline mdit_value mdit_value_cstr(const char *cstr)
{
    mdit_str s; s.data = cstr; s.len = 0;
    if (cstr != NULL) {
        const char *q = cstr;
        while (*q) ++q;
        s.len = (size_t)(q - cstr);
    }
    mdit_value v;
    v.kind = MDIT_VALUE_STR;
    v.u.s = s;
    return v;
}

/* ---------------------------------------------------------------------
 * mdit_map: ordered (key -> mdit_value) pairs.
 *
 * Operations:
 *   - get / set (insertion order preserved on first set)
 *   - has
 *   - delete (preserves relative order of other entries)
 *   - len, iterate (by index)
 *   - clear (does not free the backing storage)
 * ------------------------------------------------------------------- */
typedef struct mdit_map_entry {
    mdit_str   key;
    mdit_value value;
} mdit_map_entry;

typedef struct mdit_map {
    mdit_map_entry *data;
    size_t          len;
    size_t          cap;
    mdit_lib_ctx   *lib;      /* required when arena != NULL */
    mdit_arena     *arena;   /* NULL = malloc-backed */
} mdit_map;

void  mdit_map_init    (mdit_map *m, mdit_lib_ctx *lib, mdit_arena *arena);
void  mdit_map_destroy (mdit_map *m);
void  mdit_map_clear   (mdit_map *m);
size_t mdit_map_len    (const mdit_map *m);
const mdit_map_entry *mdit_map_at(const mdit_map *m, size_t i);

bool  mdit_map_has     (const mdit_map *m, mdit_str key);
const mdit_value *mdit_map_get(const mdit_map *m, mdit_str key);
const mdit_value *mdit_map_get_z(const mdit_map *m, const char *key);

/* set inserts at the end if key is new, or overwrites in-place otherwise.
 * Returns true on success, false on allocation failure. */
bool  mdit_map_set     (mdit_map *m, mdit_str key, mdit_value value);
bool  mdit_map_set_z   (mdit_map *m, const char *key, mdit_value value);

/* delete removes an entry preserving the relative order of the rest.
 * Returns true if a deletion happened. */
bool  mdit_map_del     (mdit_map *m, mdit_str key);

#ifdef __cplusplus
}
#endif

#endif /* MDIT_SRC_MAP_H */
