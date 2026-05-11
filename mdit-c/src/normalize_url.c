#include "normalize_url.h"

#include <string.h>

#include "json.h"
#include "punycode.h"
#include "url.h"

static mdit_str arena_copy(mdit_lib_ctx *lib, mdit_arena *arena,
                           const char *data, size_t len)
{
    if (len == 0) return MDIT_STR_LIT("");
    char *buf = (char *)mdit_arena_alloc(lib, arena, len);
    memcpy(buf, data, len);
    return (mdit_str){ buf, len };
}

static unsigned char ascii_lower(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c;
}

static bool starts_with_ci(mdit_str s, const char *prefix)
{
    size_t n = strlen(prefix);
    if (s.len < n) return false;
    for (size_t i = 0; i < n; ++i) {
        if (ascii_lower((unsigned char)s.data[i]) !=
            ascii_lower((unsigned char)prefix[i])) {
            return false;
        }
    }
    return true;
}

static mdit_str ascii_strip(mdit_str s)
{
    size_t lo = 0, hi = s.len;
    while (lo < hi) {
        unsigned char c = (unsigned char)s.data[lo];
        if (c == ' ' || (c >= 0x09 && c <= 0x0D)) ++lo;
        else break;
    }
    while (hi > lo) {
        unsigned char c = (unsigned char)s.data[hi - 1];
        if (c == ' ' || (c >= 0x09 && c <= 0x0D)) --hi;
        else break;
    }
    return (mdit_str){ s.data + lo, hi - lo };
}

/* True iff the parsed URL has a hostname we should run through IDN
 * (matches upstream's RECODE_HOSTNAME_FOR predicate). Unknown schemes
 * are skipped because the parser may have misclassified a path-like
 * component as a host (e.g. ``skype:name`` -> host=name). */
static bool should_idn(const mdit_url *parsed)
{
    if (!parsed->has_hostname || parsed->hostname.len == 0) return false;
    if (!parsed->has_protocol) return true;
    return mdit_str_eq_ci(parsed->protocol, "http:")  ||
           mdit_str_eq_ci(parsed->protocol, "https:") ||
           mdit_str_eq_ci(parsed->protocol, "mailto:");
}

bool mdit_normalize_link(mdit_lib_ctx *lib, mdit_arena *arena,
                         mdit_str url, mdit_str *out)
{
    mdit_url parsed;
    if (!mdit_url_parse(lib, arena, url, true, &parsed)) return false;

    if (should_idn(&parsed)) {
        mdit_str ascii_host;
        if (mdit_idn_to_ascii(lib, arena, parsed.hostname, &ascii_host)) {
            parsed.hostname = ascii_host;
        }
        /* On failure (OOM only, in practice), keep the original
         * hostname — matches Python's ``with suppress(Exception)``. */
    }

    mdit_buf formatted;
    mdit_buf_init(&formatted, lib);
    bool ok = mdit_url_format(&parsed, &formatted);
    mdit_buf encoded;
    mdit_buf_init(&encoded, lib);
    if (ok) {
        ok = mdit_url_encode(
            (mdit_str){ formatted.data ? formatted.data : "", formatted.len },
            MDIT_URL_ENCODE_DEFAULT_CHARS,
            true,
            &encoded);
    }
    if (ok) {
        *out = arena_copy(lib, arena, encoded.data ? encoded.data : "",
                          encoded.len);
    }
    mdit_buf_destroy(&encoded);
    mdit_buf_destroy(&formatted);
    return ok;
}

bool mdit_normalize_link_text(mdit_lib_ctx *lib, mdit_arena *arena,
                              mdit_str url, mdit_str *out)
{
    mdit_url parsed;
    if (!mdit_url_parse(lib, arena, url, true, &parsed)) return false;

    if (should_idn(&parsed)) {
        mdit_str unicode_host;
        if (mdit_idn_to_unicode(lib, arena, parsed.hostname, &unicode_host)) {
            parsed.hostname = unicode_host;
        }
    }

    mdit_buf formatted;
    mdit_buf_init(&formatted, lib);
    bool ok = mdit_url_format(&parsed, &formatted);
    mdit_buf decoded;
    mdit_buf_init(&decoded, lib);
    if (ok) {
        ok = mdit_url_decode(
            (mdit_str){ formatted.data ? formatted.data : "", formatted.len },
            MDIT_URL_DECODE_DEFAULT_CHARS "%",
            &decoded);
    }
    if (ok) {
        *out = arena_copy(lib, arena, decoded.data ? decoded.data : "",
                          decoded.len);
    }
    mdit_buf_destroy(&decoded);
    mdit_buf_destroy(&formatted);
    return ok;
}

bool mdit_validate_link(mdit_str url)
{
    mdit_str s = ascii_strip(url);
    if (starts_with_ci(s, "vbscript:") ||
        starts_with_ci(s, "javascript:") ||
        starts_with_ci(s, "file:") ||
        starts_with_ci(s, "data:")) {
        return starts_with_ci(s, "data:image/gif;") ||
               starts_with_ci(s, "data:image/png;") ||
               starts_with_ci(s, "data:image/jpeg;") ||
               starts_with_ci(s, "data:image/webp;");
    }
    return true;
}
