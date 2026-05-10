/*
 * token.h — the parser's central data type.
 *
 * Mirrors ``markdown_it.token.Token`` field-for-field. Tokens live in
 * a parser-owned arena; ``mdit_token_*`` setters never copy strings
 * unless explicitly noted (callers pass borrowed mdit_str views into
 * arena memory, or static literals for type/tag).
 *
 * Serialization: ``mdit_token_to_json`` emits bytes equivalent to
 * Python's ``json.dumps(token.as_dict(as_upstream=True),
 * ensure_ascii=False)``. The byte-for-byte match is the contract we
 * use to cross-check the C parser against the oracle in Phase 2+.
 */
#ifndef MDIT_SRC_TOKEN_H
#define MDIT_SRC_TOKEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "json.h"
#include "map.h"
#include "str.h"
#include "vec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration so ``children`` can point at sibling tokens. */
struct mdit_token;

/*
 * Source-line range. Python uses ``map: list[int] | None``; we keep
 * the same null-vs-set distinction via ``has_map``. ``begin`` is
 * inclusive, ``end`` is exclusive (matches upstream).
 */
typedef struct mdit_token_map {
    int32_t begin;
    int32_t end;
} mdit_token_map;

typedef struct mdit_token {
    /* Borrowed views. ``type`` and ``tag`` are typically static
     * literals (e.g. "paragraph_open", "p"); ``content`` / ``markup`` /
     * ``info`` come from arena-owned slabs. */
    mdit_str  type;
    mdit_str  tag;
    mdit_str  content;
    mdit_str  markup;
    mdit_str  info;

    int8_t    nesting;     /* -1, 0, +1 */
    int32_t   level;
    bool      block;
    bool      hidden;

    bool             has_map;
    mdit_token_map   map;

    mdit_map  attrs;       /* ordered string -> mdit_value */
    mdit_map  meta;        /* plugin-defined arbitrary data */

    /* Children: NULL means Python's ``None``; an empty but allocated
     * vector means an empty array (rare but the parser does produce
     * those). The vector is arena-owned. */
    struct mdit_token *children;
    size_t             children_len;
    size_t             children_cap;

    /* Backing arena for any growth (attrs/meta/children). Borrowed. */
    mdit_lib_ctx *lib;
    mdit_arena   *arena;
} mdit_token;

/* Typed dynamic array of tokens — used by the parsers and by callers
 * who consume the parsed token stream. The vec is arena-backed when
 * its `init` is called with a non-NULL arena, otherwise malloc-backed.
 */
MDIT_VEC_DECLARE(token, mdit_token)

/* ---------------------------------------------------------------------
 * Construction / destruction
 *
 * Tokens themselves live in an arena; init writes the default field
 * values into a caller-provided slot. There is no destroy: arena reset
 * tears the whole tree down at once.
 * ------------------------------------------------------------------- */
void mdit_token_init(mdit_token *t, mdit_lib_ctx *lib, mdit_arena *arena,
                     mdit_str type, mdit_str tag, int8_t nesting);

/* Convenience: allocate-and-init a single token in the arena. */
mdit_token *mdit_token_new(mdit_lib_ctx *lib, mdit_arena *arena,
                           mdit_str type, mdit_str tag, int8_t nesting);

/* ---------------------------------------------------------------------
 * Field setters that mirror upstream Token methods.
 *
 * String setters take borrowed views and do *not* copy. Callers that
 * want owned storage should ``mdit_arena_dup`` first.
 * ------------------------------------------------------------------- */
void mdit_token_set_content(mdit_token *t, mdit_str content);
void mdit_token_set_markup (mdit_token *t, mdit_str markup);
void mdit_token_set_info   (mdit_token *t, mdit_str info);
void mdit_token_set_map    (mdit_token *t, int32_t begin, int32_t end);
void mdit_token_clear_map  (mdit_token *t);

/* ---------------------------------------------------------------------
 * Attribute helpers (delegates to mdit_map).
 *
 * ``mdit_token_attr_join`` mirrors upstream ``Token.attrJoin``: appends
 * with a single space separator; raises (asserts in debug, returns
 * false in release) if the existing attr value isn't a string.
 * ------------------------------------------------------------------- */
bool  mdit_token_attr_set (mdit_token *t, mdit_str key, mdit_value value);
bool  mdit_token_attr_set_z(mdit_token *t, const char *key, mdit_value value);
const mdit_value *mdit_token_attr_get(const mdit_token *t, mdit_str key);
const mdit_value *mdit_token_attr_get_z(const mdit_token *t, const char *key);
bool  mdit_token_attr_join(mdit_token *t, mdit_str key, mdit_str value);

/* ---------------------------------------------------------------------
 * Children manipulation. Children are tokens, not pointers, so the
 * vector storage is arena-owned and grows via ``mdit_arena_try_extend``.
 * ------------------------------------------------------------------- */
mdit_token *mdit_token_push_child(mdit_token *t,
                                  mdit_str type, mdit_str tag, int8_t nesting);
size_t      mdit_token_children_len(const mdit_token *t);
mdit_token *mdit_token_child_at(mdit_token *t, size_t i);

/*
 * Force ``children`` into the "non-None empty list" state. The parser
 * occasionally sets ``token.children = []`` (notably the inline rule's
 * balance_pairs pass) and the JSON oracle distinguishes that from
 * ``None``. After this call, ``mdit_token_to_json`` emits ``[]``
 * rather than ``null`` for the children field.
 */
void mdit_token_set_children_empty(mdit_token *t);

/* ---------------------------------------------------------------------
 * JSON serialization.
 *
 * Emits the equivalent of:
 *   json.dumps(token.as_dict(as_upstream=True), ensure_ascii=False)
 * Field order matches the Python dataclass declaration order.
 *
 * Returns true on success / false on allocation failure.
 * ------------------------------------------------------------------- */
bool mdit_token_to_json (const mdit_token *t, mdit_buf *out);

/* Serialize an array of tokens as a JSON array (`[t1, t2, ...]`),
 * matching `json.dumps([t.as_dict(...) for t in tokens])` byte-for-
 * byte. Useful for the end-to-end token oracle. */
bool mdit_tokens_to_json(const mdit_token *arr, size_t n, mdit_buf *out);

#ifdef __cplusplus
}
#endif

#endif /* MDIT_SRC_TOKEN_H */
