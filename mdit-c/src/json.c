/*
 * json.c â€” JSON encoder implementation.
 *
 * Reference for the escape rules: CPython 3.12's ``Lib/json/encoder.py``
 * function ``encode_basestring`` (the slow Python path; the C
 * accelerator in ``_json.c`` matches it). We mirror its behaviour for
 * ensure_ascii=False because that is what the oracle script in
 * ``mdit-c/scripts/token_oracle.py`` uses to capture the golden HTML
 * + token streams.
 */
#include "json.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Modern MSVC (>=2015) ships a conformant snprintf; we don't need
 * _snprintf_s with its different signature. */

/* ---------------------------------------------------------------------
 * Buffer
 * ------------------------------------------------------------------- */
void mdit_buf_init(mdit_buf *b)
{
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

void mdit_buf_destroy(mdit_buf *b)
{
    if (b == NULL) return;
    free(b->data);
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

bool mdit_buf_reserve(mdit_buf *b, size_t want)
{
    /* +1 for the NUL we always keep at b->data[b->len]. */
    if (want + 1 <= b->cap) return true;
    size_t cap = b->cap < 64 ? 64 : b->cap;
    while (cap < want + 1) {
        if (cap > (size_t)-1 / 2) return false;
        cap *= 2;
    }
    char *fresh = (char *)realloc(b->data, cap);
    if (fresh == NULL) return false;
    b->data = fresh;
    b->cap  = cap;
    return true;
}

bool mdit_buf_append(mdit_buf *b, const char *s, size_t n)
{
    if (n == 0) return true;
    if (!mdit_buf_reserve(b, b->len + n)) return false;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return true;
}

bool mdit_buf_append_byte(mdit_buf *b, char c)
{
    if (!mdit_buf_reserve(b, b->len + 1)) return false;
    b->data[b->len++] = c;
    b->data[b->len]   = '\0';
    return true;
}

bool mdit_buf_appendf(mdit_buf *b, const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return false;
    }
    if (!mdit_buf_reserve(b, b->len + (size_t)n)) {
        va_end(ap2);
        return false;
    }
    int written = vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap2);
    va_end(ap2);
    if (written < 0) return false;
    b->len += (size_t)written;
    return true;
}

const char *mdit_buf_str(const mdit_buf *b)
{
    return b->data ? b->data : "";
}

size_t mdit_buf_len(const mdit_buf *b)
{
    return b->len;
}

void mdit_buf_reset(mdit_buf *b)
{
    b->len = 0;
    if (b->data && b->cap > 0) b->data[0] = '\0';
}

/* ---------------------------------------------------------------------
 * JSON value emitters
 * ------------------------------------------------------------------- */
bool mdit_json_emit_null(mdit_buf *b)
{
    return mdit_buf_append(b, "null", 4);
}

bool mdit_json_emit_bool(mdit_buf *b, bool v)
{
    return v ? mdit_buf_append(b, "true", 4)
             : mdit_buf_append(b, "false", 5);
}

bool mdit_json_emit_int(mdit_buf *b, int64_t v)
{
    char tmp[32];
    int n = snprintf(tmp, sizeof tmp, "%" PRId64, v);
    if (n < 0) return false;
    return mdit_buf_append(b, tmp, (size_t)n);
}

bool mdit_json_emit_str(mdit_buf *b, const char *s, size_t n)
{
    if (!mdit_buf_append_byte(b, '"')) return false;
    /* Pass UTF-8 through verbatim; only escape the JSON-mandatory
     * characters and ASCII control codes. */
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            if (!mdit_buf_append_byte(b, '\\')) return false;
            if (!mdit_buf_append_byte(b, (char)c)) return false;
            continue;
        }
        if (c < 0x20u) {
            switch (c) {
                case '\b': if (!mdit_buf_append(b, "\\b", 2)) return false; break;
                case '\f': if (!mdit_buf_append(b, "\\f", 2)) return false; break;
                case '\n': if (!mdit_buf_append(b, "\\n", 2)) return false; break;
                case '\r': if (!mdit_buf_append(b, "\\r", 2)) return false; break;
                case '\t': if (!mdit_buf_append(b, "\\t", 2)) return false; break;
                default: {
                    char esc[8];
                    int k = snprintf(esc, sizeof esc, "\\u%04x", c);
                    if (k < 0 || !mdit_buf_append(b, esc, (size_t)k)) {
                        return false;
                    }
                    break;
                }
            }
            continue;
        }
        if (!mdit_buf_append_byte(b, (char)c)) return false;
    }
    return mdit_buf_append_byte(b, '"');
}
