/*
 * token.c — Token struct + JSON serializer.
 */
#include "token.h"

#include <assert.h>
#include <string.h>

#include "vec.h"

MDIT_VEC_DEFINE(token, mdit_token)

/* ---------------------------------------------------------------------
 * Construction
 * ------------------------------------------------------------------- */
void mdit_token_init(mdit_token *t, mdit_lib_ctx *lib, mdit_arena *arena,
                     mdit_str type, mdit_str tag, int8_t nesting)
{
    memset(t, 0, sizeof *t);
    t->type    = type;
    t->tag     = tag;
    t->nesting = nesting;
    t->lib     = lib;
    t->arena   = arena;
    /* attrs / meta start empty; lazy-init the storage on first set. */
    mdit_map_init(&t->attrs, lib, arena);
    mdit_map_init(&t->meta, lib, arena);
}

mdit_token *mdit_token_new(mdit_lib_ctx *lib, mdit_arena *arena,
                           mdit_str type, mdit_str tag, int8_t nesting)
{
    mdit_token *t =
        (mdit_token *)mdit_arena_alloc(lib, arena, sizeof(mdit_token));
    mdit_token_init(t, lib, arena, type, tag, nesting);
    return t;
}

/* ---------------------------------------------------------------------
 * Setters
 * ------------------------------------------------------------------- */
void mdit_token_set_content(mdit_token *t, mdit_str content) { t->content = content; }
void mdit_token_set_markup (mdit_token *t, mdit_str markup)  { t->markup  = markup; }
void mdit_token_set_info   (mdit_token *t, mdit_str info)    { t->info    = info; }

void mdit_token_set_map(mdit_token *t, int32_t begin, int32_t end)
{
    t->has_map   = true;
    t->map.begin = begin;
    t->map.end   = end;
}

void mdit_token_clear_map(mdit_token *t)
{
    t->has_map   = false;
    t->map.begin = 0;
    t->map.end   = 0;
}

/* ---------------------------------------------------------------------
 * Attributes
 * ------------------------------------------------------------------- */
bool mdit_token_attr_set(mdit_token *t, mdit_str key, mdit_value value)
{
    return mdit_map_set(&t->attrs, key, value);
}

bool mdit_token_attr_set_z(mdit_token *t, const char *key, mdit_value value)
{
    return mdit_map_set_z(&t->attrs, key, value);
}

const mdit_value *mdit_token_attr_get(const mdit_token *t, mdit_str key)
{
    return mdit_map_get(&t->attrs, key);
}

const mdit_value *mdit_token_attr_get_z(const mdit_token *t, const char *key)
{
    return mdit_map_get_z(&t->attrs, key);
}

bool mdit_token_attr_join(mdit_token *t, mdit_str key, mdit_str value)
{
    const mdit_value *existing = mdit_map_get(&t->attrs, key);
    if (existing == NULL) {
        return mdit_map_set(&t->attrs, key, mdit_value_str(value));
    }
    if (existing->kind != MDIT_VALUE_STR) {
        /* Upstream raises TypeError; we degrade gracefully so plugins
         * with funky attr types don't crash the parser. Programming
         * errors should still trip the assert in debug builds. */
        assert(0 && "attr_join: existing attr is not a string");
        return false;
    }
    /* Concatenate ``existing.s + ' ' + value`` into a fresh arena slot. */
    size_t total = existing->u.s.len + 1 + value.len;
    char *buf = (char *)mdit_arena_alloc(t->lib, t->arena, total);
    memcpy(buf, existing->u.s.data, existing->u.s.len);
    buf[existing->u.s.len] = ' ';
    memcpy(buf + existing->u.s.len + 1, value.data, value.len);
    mdit_str joined; joined.data = buf; joined.len = total;
    return mdit_map_set(&t->attrs, key, mdit_value_str(joined));
}

/* ---------------------------------------------------------------------
 * Children
 * ------------------------------------------------------------------- */
static bool grow_children(mdit_token *t, size_t want_cap)
{
    if (want_cap <= t->children_cap) return true;
    size_t cap = t->children_cap < 4 ? 4 : t->children_cap + (t->children_cap >> 1);
    if (cap < want_cap) cap = want_cap;
    size_t bytes = cap * sizeof(mdit_token);

    if (t->children == NULL) {
        t->children = (mdit_token *)mdit_arena_alloc(t->lib, t->arena, bytes);
        t->children_cap = cap;
        return true;
    }
    /* Try to extend in place at the arena tail. */
    size_t old_bytes = t->children_cap * sizeof(mdit_token);
    void *p = t->children;
    if (mdit_arena_try_extend(t->lib, t->arena, &p, old_bytes, bytes)) {
        t->children     = (mdit_token *)p;
        t->children_cap = cap;
        return true;
    }
    /* Otherwise, allocate fresh and copy. The previous slab is leaked
     * into the arena until reset(). */
    mdit_token *fresh = (mdit_token *)mdit_arena_alloc(t->lib, t->arena, bytes);
    if (t->children_len > 0) {
        memcpy(fresh, t->children, t->children_len * sizeof(mdit_token));
    }
    t->children     = fresh;
    t->children_cap = cap;
    return true;
}

mdit_token *mdit_token_push_child(mdit_token *t,
                                  mdit_str type, mdit_str tag, int8_t nesting)
{
    if (t->children_len == t->children_cap) {
        if (!grow_children(t, t->children_len + 1)) return NULL;
    }
    mdit_token *slot = &t->children[t->children_len++];
    mdit_token_init(slot, t->lib, t->arena, type, tag, nesting);
    return slot;
}

size_t mdit_token_children_len(const mdit_token *t)
{
    return t->children_len;
}

mdit_token *mdit_token_child_at(mdit_token *t, size_t i)
{
    if (i >= t->children_len) return NULL;
    return &t->children[i];
}

void mdit_token_set_children_empty(mdit_token *t)
{
    if (t->children != NULL) return; /* already non-None */
    /* Allocate a single sentinel slot so the pointer is non-NULL but
     * len stays at 0. The slot is never read; the JSON path only
     * iterates [0, children_len). */
    (void)grow_children(t, 1);
}

/* ---------------------------------------------------------------------
 * JSON serialization
 *
 * Field order MUST match the Python dataclass declaration in
 * markdown_it/token.py: type, tag, nesting, attrs, map, level,
 * children, content, markup, info, meta, block, hidden.
 *
 * `attrs` follows the upstream representation: ``null`` if empty,
 * otherwise a list of ``[key, value]`` pairs. `meta` always renders
 * as an object (matching the dataclass's default ``{}``).
 * ------------------------------------------------------------------- */
static bool emit_value(mdit_buf *b, const mdit_value *v);

static bool emit_str_view(mdit_buf *b, mdit_str s)
{
    return mdit_json_emit_str(b, s.data, s.len);
}

static bool emit_attrs(mdit_buf *b, const mdit_map *m)
{
    if (mdit_map_len(m) == 0) {
        return mdit_json_emit_null(b);
    }
    if (!mdit_buf_append_byte(b, '[')) return false;
    for (size_t i = 0; i < mdit_map_len(m); ++i) {
        const mdit_map_entry *e = mdit_map_at(m, i);
        if (i > 0 && !mdit_buf_append(b, ", ", 2)) return false;
        if (!mdit_buf_append_byte(b, '[')) return false;
        if (!emit_str_view(b, e->key)) return false;
        if (!mdit_buf_append(b, ", ", 2)) return false;
        if (!emit_value(b, &e->value)) return false;
        if (!mdit_buf_append_byte(b, ']')) return false;
    }
    return mdit_buf_append_byte(b, ']');
}

static bool emit_meta(mdit_buf *b, const mdit_map *m)
{
    if (!mdit_buf_append_byte(b, '{')) return false;
    for (size_t i = 0; i < mdit_map_len(m); ++i) {
        const mdit_map_entry *e = mdit_map_at(m, i);
        if (i > 0 && !mdit_buf_append(b, ", ", 2)) return false;
        if (!emit_str_view(b, e->key)) return false;
        if (!mdit_buf_append(b, ": ", 2)) return false;
        if (!emit_value(b, &e->value)) return false;
    }
    return mdit_buf_append_byte(b, '}');
}

static bool emit_value(mdit_buf *b, const mdit_value *v)
{
    switch (v->kind) {
        case MDIT_VALUE_NULL:   return mdit_json_emit_null(b);
        case MDIT_VALUE_BOOL:   return mdit_json_emit_bool(b, v->u.b);
        case MDIT_VALUE_INT:    return mdit_json_emit_int (b, v->u.i);
        case MDIT_VALUE_DOUBLE: {
            /* Python's float repr is non-trivial (shortest round-trip);
             * we don't currently need it because no upstream rule
             * stores floats in attrs/meta. Punt to %.17g for now and
             * add a TODO marker — when a rule needs floats, port the
             * shortest-repr algorithm. */
            return mdit_buf_appendf(b, "%.17g", v->u.d);
        }
        case MDIT_VALUE_STR:    return emit_str_view(b, v->u.s);
    }
    return false;
}

static bool emit_field_str(mdit_buf *b, const char *name, mdit_str s,
                           bool first)
{
    if (!first && !mdit_buf_append(b, ", ", 2)) return false;
    if (!mdit_json_emit_str(b, name, strlen(name))) return false;
    if (!mdit_buf_append(b, ": ", 2)) return false;
    return emit_str_view(b, s);
}

bool mdit_token_to_json(const mdit_token *t, mdit_buf *out)
{
    if (!mdit_buf_append_byte(out, '{')) return false;

    if (!emit_field_str(out, "type", t->type, true)) return false;
    if (!emit_field_str(out, "tag",  t->tag,  false)) return false;

    if (!mdit_buf_append(out, ", ", 2)) return false;
    if (!mdit_json_emit_str(out, "nesting", 7)) return false;
    if (!mdit_buf_append(out, ": ", 2)) return false;
    if (!mdit_json_emit_int(out, t->nesting)) return false;

    if (!mdit_buf_append(out, ", ", 2)) return false;
    if (!mdit_json_emit_str(out, "attrs", 5)) return false;
    if (!mdit_buf_append(out, ": ", 2)) return false;
    if (!emit_attrs(out, &t->attrs)) return false;

    if (!mdit_buf_append(out, ", ", 2)) return false;
    if (!mdit_json_emit_str(out, "map", 3)) return false;
    if (!mdit_buf_append(out, ": ", 2)) return false;
    if (!t->has_map) {
        if (!mdit_json_emit_null(out)) return false;
    } else {
        if (!mdit_buf_append_byte(out, '[')) return false;
        if (!mdit_json_emit_int(out, t->map.begin)) return false;
        if (!mdit_buf_append(out, ", ", 2)) return false;
        if (!mdit_json_emit_int(out, t->map.end)) return false;
        if (!mdit_buf_append_byte(out, ']')) return false;
    }

    if (!mdit_buf_append(out, ", ", 2)) return false;
    if (!mdit_json_emit_str(out, "level", 5)) return false;
    if (!mdit_buf_append(out, ": ", 2)) return false;
    if (!mdit_json_emit_int(out, t->level)) return false;

    if (!mdit_buf_append(out, ", ", 2)) return false;
    if (!mdit_json_emit_str(out, "children", 8)) return false;
    if (!mdit_buf_append(out, ": ", 2)) return false;
    if (t->children == NULL) {
        if (!mdit_json_emit_null(out)) return false;
    } else {
        if (!mdit_buf_append_byte(out, '[')) return false;
        for (size_t i = 0; i < t->children_len; ++i) {
            if (i > 0 && !mdit_buf_append(out, ", ", 2)) return false;
            if (!mdit_token_to_json(&t->children[i], out)) return false;
        }
        if (!mdit_buf_append_byte(out, ']')) return false;
    }

    if (!emit_field_str(out, "content", t->content, false)) return false;
    if (!emit_field_str(out, "markup",  t->markup,  false)) return false;
    if (!emit_field_str(out, "info",    t->info,    false)) return false;

    if (!mdit_buf_append(out, ", ", 2)) return false;
    if (!mdit_json_emit_str(out, "meta", 4)) return false;
    if (!mdit_buf_append(out, ": ", 2)) return false;
    if (!emit_meta(out, &t->meta)) return false;

    if (!mdit_buf_append(out, ", ", 2)) return false;
    if (!mdit_json_emit_str(out, "block", 5)) return false;
    if (!mdit_buf_append(out, ": ", 2)) return false;
    if (!mdit_json_emit_bool(out, t->block)) return false;

    if (!mdit_buf_append(out, ", ", 2)) return false;
    if (!mdit_json_emit_str(out, "hidden", 6)) return false;
    if (!mdit_buf_append(out, ": ", 2)) return false;
    if (!mdit_json_emit_bool(out, t->hidden)) return false;

    return mdit_buf_append_byte(out, '}');
}

bool mdit_tokens_to_json(const mdit_token *arr, size_t n, mdit_buf *out)
{
    if (!mdit_buf_append_byte(out, '[')) return false;
    for (size_t i = 0; i < n; ++i) {
        if (i > 0 && !mdit_buf_append(out, ", ", 2)) return false;
        if (!mdit_token_to_json(&arr[i], out)) return false;
    }
    return mdit_buf_append_byte(out, ']');
}
