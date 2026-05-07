/*
 * escape.h — HTML / Markdown escape helpers used by the renderer.
 *
 * Mirrors a small subset of ``markdown_it.common.utils``:
 *
 *   ``escapeHtml``  — replace & < > " with their named entities.
 *   ``unescapeAll`` — undo CommonMark backslash escapes and named
 *                     /numeric HTML entities, i.e. the post-parse
 *                     normalization render rules apply to text and
 *                     code content.
 *   ``isValidEntityCode`` — guard for numeric character references.
 *   ``fromCodePoint`` — encode a codepoint as UTF-8.
 *
 * All output is appended to a ``mdit_buf`` so callers can compose
 * these helpers with the JSON / URL writers without intermediate
 * allocations.
 */
#ifndef MDIT_SRC_ESCAPE_H
#define MDIT_SRC_ESCAPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "json.h"
#include "str.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Append ``input`` (UTF-8) to ``out``, replacing &, <, >, and " with
 * their named entity equivalents. Single quotes are NOT escaped
 * (matches markdown-it / Python's behaviour).
 */
bool mdit_escape_html(mdit_str input, mdit_buf *out);

/*
 * CommonMark "backslash escape OR HTML entity" replacement. Bytes that
 * don't participate in either pass through untouched. Returns false on
 * allocation failure.
 *
 * The shape of the regex this replaces:
 *
 *     UNESCAPE_ALL_RE = (
 *         r'\\([!"#$%&\'()*+,\-./:;<=>?@\[\\\]^_`{|}~])'
 *         + "|"
 *         + r"&([a-z#][a-z0-9]{1,31});"
 *     )
 *
 * For each match, ``\X`` becomes ``X`` and ``&name;`` becomes the
 * corresponding entity expansion (named or numeric). If the entity
 * name is unknown / invalid, the original sequence is preserved
 * verbatim — same as upstream.
 */
bool mdit_unescape_all(mdit_str input, mdit_buf *out);

/*
 * Validate a numeric character reference codepoint. Mirrors
 * ``isValidEntityCode`` from upstream: rejects surrogates, C0/C1
 * controls, U+FFFE/U+FFFF + plane equivalents, the noncharacter
 * U+FDD0..U+FDEF range, and codepoints above U+10FFFF.
 */
bool mdit_is_valid_entity_code(uint32_t cp);

/*
 * Encode ``cp`` as UTF-8 into ``out``. ``cp`` must be a valid Unicode
 * scalar (the caller is expected to have run ``isValidEntityCode``
 * first or to know it's safe). Returns false on allocation failure or
 * an out-of-range codepoint.
 */
bool mdit_emit_utf8(uint32_t cp, mdit_buf *out);

#ifdef __cplusplus
}
#endif

#endif /* MDIT_SRC_ESCAPE_H */
