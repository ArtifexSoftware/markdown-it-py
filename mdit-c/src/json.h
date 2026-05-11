/*
 * json.h — a tiny JSON encoder for the oracle / token serializer.
 *
 * The C port doesn't need a general-purpose JSON library; it needs a
 * writer that emits the *same bytes* Python's ``json.dumps(...,
 * ensure_ascii=False)`` would, for the value subset that ``Token``
 * uses (null / bool / int / double / string / object / array).
 *
 * The grow-only buffer (``mdit_buf``) is host-backed via ``mdit_lib_ctx``
 * hooks; the caller frees it via ``mdit_buf_destroy``. We don't route
 * this through the arena because the rendered JSON typically outlives
 * the parser state (it's consumed by tests / FFI).
 */
#ifndef MDIT_SRC_JSON_H
#define MDIT_SRC_JSON_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mdit/mdit.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mdit_buf {
    mdit_lib_ctx *lib;
    char         *data;
    size_t        len;
    size_t        cap;
} mdit_buf;

void   mdit_buf_init   (mdit_buf *b, mdit_lib_ctx *lib);
void   mdit_buf_destroy(mdit_buf *b);

/* Convenience for harnesses that do not own a custom allocator. */
static inline void mdit_buf_init_default(mdit_buf *b)
{
    mdit_buf_init(b, (mdit_lib_ctx *)(void *)mdit_lib_ctx_builtin_default());
}
bool   mdit_buf_reserve(mdit_buf *b, size_t want);
bool   mdit_buf_append (mdit_buf *b, const char *s, size_t n);
bool   mdit_buf_append_byte(mdit_buf *b, char c);
#if defined(__GNUC__) || defined(__clang__)
bool mdit_buf_appendf(mdit_buf *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
bool   mdit_buf_appendf(mdit_buf *b, const char *fmt, ...);
#endif

/* Truncate the buffer's logical length back to zero without freeing
 * capacity. Useful for reusing scratch buffers (e.g. the inline state's
 * `pending` text accumulator between flushes). */
void   mdit_buf_reset  (mdit_buf *b);

/* Length-tracked NUL-terminated view of the buffer's current contents. */
const char *mdit_buf_str(const mdit_buf *b);
size_t      mdit_buf_len(const mdit_buf *b);

/* ---------------------------------------------------------------------
 * JSON value emitters.
 *
 * Every emitter writes directly into ``b`` and returns ``true`` on
 * success / ``false`` on allocation failure. Strings are emitted with
 * Python ``json.dumps`` escape rules under ``ensure_ascii=False``:
 *
 *   - "  -> \"
 *   - \  -> \\
 *   - control chars (< 0x20) -> \uXXXX
 *   - everything else (incl. valid UTF-8 multibyte) passes through.
 *
 * We do NOT escape forward slashes; Python's default doesn't either.
 * ------------------------------------------------------------------- */
bool mdit_json_emit_null  (mdit_buf *b);
bool mdit_json_emit_bool  (mdit_buf *b, bool v);
bool mdit_json_emit_int   (mdit_buf *b, int64_t v);
bool mdit_json_emit_str   (mdit_buf *b, const char *s, size_t n);

/* Object / array structural helpers. The caller drives field/element
 * separators by calling ``mdit_json_emit_sep`` between items.
 *
 * Typical use:
 *
 *     mdit_buf_append_byte(b, '{');
 *     mdit_json_emit_str(b, "key", 3);
 *     mdit_buf_append(b, ": ", 2);
 *     mdit_json_emit_int(b, 42);
 *     mdit_buf_append_byte(b, '}');
 *
 * The two-byte ", " separator is exactly what Python's default
 * ``json.dumps`` writes between fields; this header doesn't try to
 * abstract it.  */

#ifdef __cplusplus
}
#endif

#endif /* MDIT_SRC_JSON_H */
