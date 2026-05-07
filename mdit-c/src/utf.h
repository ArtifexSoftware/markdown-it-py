/*
 * utf.h — Unicode codepoint classification used by the parser hot path.
 *
 * Two predicates here:
 *
 *   ``mdit_is_punct``      — Unicode general category in {P*, S*}.
 *                            Implemented via a generated 2-level
 *                            bitmap (see scripts/gen_unicode_tables.py
 *                            and src/utf_tables.c).
 *
 *   ``mdit_is_whitespace`` — the explicit set markdown-it-py uses,
 *                            i.e. ASCII whitespace (\t \n \v \f \r ' ')
 *                            plus a handful of Zs codepoints. Tiny
 *                            enough to live in str.c-style branchy
 *                            form rather than burning a UCD table.
 *
 * Codepoint inputs are uint32_t to match ``mdit_decode``'s output.
 * Inputs outside [0, 0x110000) return false (they cannot represent a
 * legal Unicode scalar value).
 */
#ifndef MDIT_SRC_UTF_H
#define MDIT_SRC_UTF_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True iff ``cp`` has Unicode general category ``P*`` or ``S*``.
 * Mirrors ``markdown_it.common.utils.isPunctChar`` exactly. */
bool mdit_is_punct(uint32_t cp);

/* The whitespace set used by markdown-it-py:
 *   \t \n \v \f \r ' ', U+00A0, U+1680, U+2000..U+200A, U+202F,
 *   U+205F, U+3000.
 * (Notably *not* U+2028 / U+2029 — markdown-it doesn't treat them
 *  as whitespace.)  */
bool mdit_is_whitespace(uint32_t cp);

#ifdef __cplusplus
}
#endif

#endif /* MDIT_SRC_UTF_H */
