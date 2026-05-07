/*
 * url.h — port of the Python ``mdurl`` package.
 *
 * The Python ``mdurl`` library is markdown-it-py's only non-stdlib
 * runtime dependency. It descends from Joyent's Node ``url`` module
 * with markdown-it-specific tweaks (no leading slash in pathnames,
 * trailing colon stays in the path, hostname validation that strips
 * non-ASCII characters in unicode-y URLs, etc.).
 *
 * This port preserves the exact behaviour of mdurl 0.1.x — every
 * decision the Python parser makes about where the path begins, where
 * the hostname ends, and how to fall back when validation fails is
 * mirrored. We cross-check that with a generator that runs real URLs
 * through Python and emits a header of expected outputs.
 *
 * Lifetime / ownership:
 *   - ``mdit_url`` field strings are arena-owned views. The parser
 *     copies its substring extractions into the arena so callers don't
 *     have to keep the source string alive.
 *   - ``has_*`` flags distinguish "absent" (Python's ``None``) from
 *     "empty string" — that distinction matters for the formatter
 *     because ``hostname == ""`` still emits no characters but
 *     ``hostname is None`` also does not, so we keep the bit for
 *     introspection / future renderers.
 */
#ifndef MDIT_SRC_URL_H
#define MDIT_SRC_URL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "json.h"
#include "str.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mdit_url {
    bool      has_protocol;  mdit_str protocol;  /* includes trailing ':' */
    bool      slashes;
    bool      has_auth;      mdit_str auth;
    bool      has_port;      mdit_str port;
    bool      has_hostname;  mdit_str hostname;
    bool      has_hash;      mdit_str hash;      /* includes leading '#' */
    bool      has_search;    mdit_str search;    /* includes leading '?' */
    bool      has_pathname;  mdit_str pathname;
} mdit_url;

/*
 * Parse ``input`` (UTF-8) into ``out``, allocating substring views
 * from ``arena``. ``slashes_denote_host`` controls whether a leading
 * ``//`` enables host-mode parsing (matches the upstream kwarg).
 *
 * The parser never fails: any malformed-looking URL is parsed in a
 * "best effort" mode that mirrors what Python's mdurl does. The only
 * way this returns false is on out-of-memory.
 */
bool mdit_url_parse(mdit_arena *arena,
                    mdit_str input,
                    bool slashes_denote_host,
                    mdit_url *out);

/*
 * Reassemble a parsed URL into its canonical string form. Mirrors
 * ``mdurl.format``: protocol + (slashes ? "//" : "") + auth +
 * (ipv6 brackets ? "[" + hostname + "]" : hostname) + (":" + port) +
 * pathname + search + hash.
 *
 * The output is appended to ``out`` (which the caller initializes with
 * ``mdit_buf_init``) so it composes naturally into larger strings.
 */
bool mdit_url_format(const mdit_url *url, mdit_buf *out);

/*
 * Standard exclude sets. The Python module exports these as module
 * constants; we mirror the values so callers don't have to spell them
 * out. They are ASCII strings, so passing them as ``mdit_str`` is a
 * cheap literal.
 */
#define MDIT_URL_ENCODE_DEFAULT_CHARS    ";/?:@&=+$,-_.!~*'()#"
#define MDIT_URL_ENCODE_COMPONENT_CHARS  "-_.!~*'()"
#define MDIT_URL_DECODE_DEFAULT_CHARS    ";/?:@&=+$,#"
#define MDIT_URL_DECODE_COMPONENT_CHARS  ""

/*
 * Percent-encode unsafe bytes. ``exclude`` is a string of additional
 * ASCII characters that should pass through unencoded (in addition to
 * a-zA-Z0-9 which always pass through). Non-ASCII bytes in the input
 * (i.e. UTF-8 continuation / leading bytes) are encoded as %XX.
 *
 * If ``keep_escaped`` is true, an existing ``%XY`` sequence where X
 * and Y are hex digits is passed through verbatim instead of being
 * re-encoded as ``%25XY``.
 */
bool mdit_url_encode(mdit_str input,
                     const char *exclude,
                     bool keep_escaped,
                     mdit_buf *out);

/*
 * Percent-decode escape sequences. ``exclude`` is a string of ASCII
 * characters that should NOT be decoded — if a sequence resolves to an
 * excluded byte, it's emitted as ``%XX`` (uppercase) instead of the
 * raw character. Bytes outside the exclude set are decoded; invalid
 * sequences (truncated, non-hex, malformed UTF-8) emit U+FFFD per
 * codepoint, matching upstream.
 */
bool mdit_url_decode(mdit_str input,
                     const char *exclude,
                     mdit_buf *out);

#ifdef __cplusplus
}
#endif

#endif /* MDIT_SRC_URL_H */
