/*
 * str.h — UTF-8 byte views, codepoint iteration, and the small set of
 * ASCII classification helpers the parser needs in its hot path.
 *
 * The parser operates on UTF-8 byte buffers; positions are byte
 * offsets, not codepoint offsets. ``mdit_decode`` converts a single
 * codepoint plus its byte length, so loops can advance by bytes while
 * reasoning about codepoints. This matches how cmark and most C
 * Markdown implementations handle the input — and keeps memory
 * footprint at one byte per ASCII character (which most Markdown is).
 *
 * Codepoint classification (alpha / mark / number / punct / space)
 * lives in utf.h, which is generated from the Unicode Character
 * Database. This header only deals with the byte layer.
 */
#ifndef MDIT_SRC_STR_H
#define MDIT_SRC_STR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The Unicode replacement codepoint, used by the lossy decoder. */
#define MDIT_REPLACEMENT_CHAR 0xFFFDu

/* The maximum legal codepoint. */
#define MDIT_CODEPOINT_MAX    0x10FFFFu

/* ---------------------------------------------------------------------
 * Borrowed string view: pointer + length, no ownership.
 * ------------------------------------------------------------------- */
typedef struct mdit_str {
    const char *data;
    size_t      len;
} mdit_str;

#define MDIT_STR_LIT(literal) ((mdit_str){ (literal), sizeof(literal) - 1 })

static inline mdit_str mdit_str_make(const char *data, size_t len)
{
    mdit_str s; s.data = data; s.len = len; return s;
}

bool   mdit_str_eq    (mdit_str a, mdit_str b);
bool   mdit_str_eq_z  (mdit_str a, const char *cstr);
bool   mdit_str_eq_ci (mdit_str a, const char *cstr); /* ASCII-only icase */

/* ---------------------------------------------------------------------
 * UTF-8 codepoint decode / encode
 * ------------------------------------------------------------------- */

/*
 * Decode the codepoint that starts at ``data[0]``. ``len`` is the
 * number of bytes available at ``data``.
 *
 *   On success, ``*out_cp`` is the decoded codepoint and the return
 *   value is the number of bytes consumed (1..4).
 *
 *   On failure (invalid lead byte, truncated sequence, overlong, or
 *   surrogate), ``*out_cp`` is set to MDIT_REPLACEMENT_CHAR and the
 *   return value is 1 — i.e. the lossy decoder advances byte-by-byte
 *   past invalid input, exactly like Python's ``surrogateescape``-free
 *   ``utf-8`` codec does for malformed sequences in strict-error mode
 *   (we just don't raise).
 *
 *   ``len == 0`` returns 0 and writes MDIT_REPLACEMENT_CHAR.
 */
size_t mdit_decode(const char *data, size_t len, uint32_t *out_cp);

/*
 * Strict variant that returns the byte length on success and 0 on any
 * malformed input. Used by validators that want to *reject* rather
 * than lossily replace.
 */
size_t mdit_decode_strict(const char *data, size_t len, uint32_t *out_cp);

/*
 * Encode ``cp`` to UTF-8 in ``out`` (which must have ``cap`` bytes
 * available, at least 4 to be safe). Returns the number of bytes
 * written, or 0 on overflow / invalid codepoint.
 */
size_t mdit_encode(uint32_t cp, char *out, size_t cap);

/* Validate that the entire buffer is well-formed UTF-8. */
bool   mdit_is_utf8(const char *data, size_t len);

/* Number of codepoints in a well-formed UTF-8 buffer (lossy: malformed
 * sequences count one codepoint per byte advanced). */
size_t mdit_count_codepoints(const char *data, size_t len);

/* ---------------------------------------------------------------------
 * ASCII fast paths used by the inline scanners.
 *
 * These match upstream `markdown-it` semantics — note that
 * ``mdit_is_md_ascii_punct`` is the strict CommonMark ASCII punctuation
 * set, *not* C ``ispunct``. Codepoint-level punctuation lives in utf.h.
 * ------------------------------------------------------------------- */
static inline bool mdit_is_ascii(uint32_t c) { return c < 0x80; }

static inline bool mdit_is_ascii_digit(uint32_t c)
{
    return c >= '0' && c <= '9';
}

static inline bool mdit_is_ascii_lower(uint32_t c)
{
    return c >= 'a' && c <= 'z';
}

static inline bool mdit_is_ascii_upper(uint32_t c)
{
    return c >= 'A' && c <= 'Z';
}

static inline bool mdit_is_ascii_alpha(uint32_t c)
{
    return mdit_is_ascii_lower(c) || mdit_is_ascii_upper(c);
}

static inline bool mdit_is_ascii_alnum(uint32_t c)
{
    return mdit_is_ascii_alpha(c) || mdit_is_ascii_digit(c);
}

/* ASCII whitespace in the markdown-it sense: \t \n \v \f \r ' '. */
static inline bool mdit_is_ascii_space(uint32_t c)
{
    return c == 0x20 || (c >= 0x09 && c <= 0x0D);
}

/* CommonMark "ASCII punctuation": !"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~. */
bool mdit_is_md_ascii_punct(uint32_t c);

/* ASCII-only case-fold (lowercase). */
static inline uint32_t mdit_ascii_tolower(uint32_t c)
{
    return mdit_is_ascii_upper(c) ? c + 0x20 : c;
}

#ifdef __cplusplus
}
#endif

#endif /* MDIT_SRC_STR_H */
