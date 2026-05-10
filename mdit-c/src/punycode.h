/*
 * punycode.h — RFC 3492 Bootstring (Punycode) and IDN domain helpers.
 *
 * Two layers live here:
 *
 *   Low-level codec
 *     ``mdit_punycode_encode_cps`` / ``mdit_punycode_decode``: the
 *     pure RFC-3492 algorithms over codepoint arrays. They are
 *     symmetric with Python's ``codecs.encode/decode(s, "punycode")``,
 *     including the trailing ``-`` delimiter when the input has any
 *     basic (ASCII) codepoints.
 *
 *   Domain wrappers
 *     ``mdit_idn_to_ascii`` / ``mdit_idn_to_unicode``: split the input
 *     on the four IDN label separators (``.``, ``\u3002``, ``\uFF0E``,
 *     ``\uFF61``), apply the codec per label, and rejoin with ``.``.
 *     They mirror ``markdown_it._punycode.to_ascii`` /
 *     ``to_unicode`` exactly: ``to_ascii`` only encodes labels that
 *     contain non-ASCII codepoints, ``to_unicode`` only decodes labels
 *     beginning with ``xn--`` (case-insensitive). An ``@`` in the
 *     input separates the email local part — that part is passed
 *     through verbatim.
 *
 * Failure handling: the wrappers return false only on out-of-memory.
 * Codec failures on individual labels (invalid Punycode, oversized
 * codepoints, …) are swallowed and the original label is emitted, just
 * as upstream wraps the codec calls in ``with suppress(Exception)``.
 */
#ifndef MDIT_SRC_PUNYCODE_H
#define MDIT_SRC_PUNYCODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "json.h"  /* for mdit_buf */
#include "str.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Encode codepoints in ``cps[0..n)`` to Punycode ASCII bytes appended
 * to ``out``. Mirrors ``codecs.encode(s, "punycode")``: when ``n`` > 0
 * and at least one codepoint is basic (< 0x80), a ``-`` delimiter is
 * appended after the basic prefix even if the input is entirely
 * ASCII. Returns true on success. */
bool mdit_punycode_encode_cps(const uint32_t *cps, size_t n, mdit_buf *out);

/* Decode Punycode ASCII (without an ``xn--`` prefix) into codepoints
 * appended to ``out``. ``out_cps`` is appended via ``mdit_buf_append``
 * with each codepoint encoded as 4 little-endian bytes; callers
 * typically use the higher-level wrappers below. Returns false on
 * malformed input. */
bool mdit_punycode_decode_to_utf8(mdit_str input, mdit_buf *out);

/* High-level: convert ``hostname`` (a single domain, optionally with
 * an ``@``-prefixed email local part) to its IDN ASCII form. Output
 * is allocated from ``arena`` and returned via ``*out``. */
bool mdit_idn_to_ascii(mdit_lib_ctx *lib, mdit_arena *arena,
                       mdit_str hostname, mdit_str *out);

/* Inverse: decode any ``xn--`` labels back to UTF-8 Unicode. */
bool mdit_idn_to_unicode(mdit_lib_ctx *lib, mdit_arena *arena,
                         mdit_str hostname, mdit_str *out);

#ifdef __cplusplus
}
#endif

#endif /* MDIT_SRC_PUNYCODE_H */
