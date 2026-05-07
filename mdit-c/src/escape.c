/*
 * escape.c — HTML / Markdown escape helpers.
 *
 * The upstream regex
 *
 *   UNESCAPE_ALL_RE = r'\\(...)' + "|" + r"&([a-z#][a-z0-9]{1,31});"
 *
 * is hand-implemented as a one-pass scanner. We don't need the regex
 * engine for this — both alternatives have a single anchor character
 * (\\ or &) and bounded lookahead.
 */
#include "escape.h"

#include <stdlib.h>
#include <string.h>

#include "entities.h"
#include "str.h"

/* ---------------------------------------------------------------------
 * escape_html
 * ------------------------------------------------------------------- */
bool mdit_escape_html(mdit_str input, mdit_buf *out)
{
    const char *s = input.data;
    size_t      n = input.len;
    size_t      run_start = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        const char *replacement = NULL;
        size_t      replacement_len = 0;
        switch (c) {
            case '&': replacement = "&amp;";  replacement_len = 5; break;
            case '<': replacement = "&lt;";   replacement_len = 4; break;
            case '>': replacement = "&gt;";   replacement_len = 4; break;
            case '"': replacement = "&quot;"; replacement_len = 6; break;
            default: continue;
        }
        if (i > run_start) {
            if (!mdit_buf_append(out, s + run_start, i - run_start)) return false;
        }
        if (!mdit_buf_append(out, replacement, replacement_len)) return false;
        run_start = i + 1;
    }
    if (run_start < n) {
        if (!mdit_buf_append(out, s + run_start, n - run_start)) return false;
    }
    return true;
}

/* ---------------------------------------------------------------------
 * isValidEntityCode  /  mdit_emit_utf8
 * ------------------------------------------------------------------- */
bool mdit_is_valid_entity_code(uint32_t cp)
{
    if (cp >= 0xD800u && cp <= 0xDFFFu) return false;
    if (cp >= 0xFDD0u && cp <= 0xFDEFu) return false;
    if ((cp & 0xFFFFu) == 0xFFFFu) return false;
    if ((cp & 0xFFFFu) == 0xFFFEu) return false;
    if (cp <= 0x08u) return false;
    if (cp == 0x0Bu) return false;
    if (cp >= 0x0Eu && cp <= 0x1Fu) return false;
    if (cp >= 0x7Fu && cp <= 0x9Fu) return false;
    return cp <= 0x10FFFFu;
}

bool mdit_emit_utf8(uint32_t cp, mdit_buf *out)
{
    if (cp <= 0x7Fu) {
        return mdit_buf_append_byte(out, (char)cp);
    }
    if (cp <= 0x7FFu) {
        char b[2];
        b[0] = (char)(0xC0u | (cp >> 6));
        b[1] = (char)(0x80u | (cp & 0x3Fu));
        return mdit_buf_append(out, b, 2);
    }
    if (cp <= 0xFFFFu) {
        char b[3];
        b[0] = (char)(0xE0u | (cp >> 12));
        b[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        b[2] = (char)(0x80u | (cp & 0x3Fu));
        return mdit_buf_append(out, b, 3);
    }
    if (cp <= 0x10FFFFu) {
        char b[4];
        b[0] = (char)(0xF0u | (cp >> 18));
        b[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        b[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        b[3] = (char)(0x80u | (cp & 0x3Fu));
        return mdit_buf_append(out, b, 4);
    }
    return false;
}

/* ---------------------------------------------------------------------
 * unescape_all
 *
 * Scanner walks the input and:
 *   - On '\\' followed by an ESCAPABLE char (the 31-character set in
 *     the upstream regex), emit the escaped char (drop the backslash).
 *   - On '&' followed by [a-z#][a-z0-9]{1,31};, attempt entity lookup:
 *       * named   (entities table)
 *       * numeric base-10 (#NNN)
 *       * numeric base-16 (#xHHH)
 *     If the entity resolves, emit its expansion; otherwise emit the
 *     entire ``&...;`` sequence verbatim (matching upstream "if the
 *     entity is invalid, return the original match").
 *   - Anything else passes through.
 * ------------------------------------------------------------------- */
static bool is_escapable(unsigned char c)
{
    /* The 31 characters from the upstream regex group:
     * !"#$%&'()*+,-./:;<=>?@[\]^_`{|}~ */
    switch (c) {
        case '!': case '"': case '#': case '$': case '%':
        case '&': case '\'': case '(': case ')': case '*':
        case '+': case ',': case '-': case '.': case '/':
        case ':': case ';': case '<': case '=': case '>':
        case '?': case '@': case '[': case '\\': case ']':
        case '^': case '_': case '`': case '{': case '|':
        case '}': case '~':
            return true;
        default:
            return false;
    }
}

static bool entity_name_lead(unsigned char c)
{
    /* [a-z#] case insensitive (upstream uses re.IGNORECASE). */
    if (c == '#') return true;
    if (c >= 'a' && c <= 'z') return true;
    if (c >= 'A' && c <= 'Z') return true;
    return false;
}

static bool entity_name_tail(unsigned char c)
{
    /* [a-z0-9] case insensitive. */
    if (c >= '0' && c <= '9') return true;
    if (c >= 'a' && c <= 'z') return true;
    if (c >= 'A' && c <= 'Z') return true;
    return false;
}

static int hex_to_int(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

/* Resolve a name extracted from ``&NAME;`` into either an entity
 * expansion appended to ``out``, or NULL on failure (caller emits the
 * original match verbatim). Returns true if appended successfully. */
static bool resolve_entity(const char *name, size_t len, mdit_buf *out)
{
    if (len == 0) return false;
    if (name[0] == '#') {
        if (len < 2) return false;
        uint32_t cp;
        if (name[1] == 'x' || name[1] == 'X') {
            if (len < 3 || len > 10) return false;
            cp = 0;
            for (size_t i = 2; i < len; ++i) {
                int v = hex_to_int((unsigned char)name[i]);
                if (v < 0) return false;
                cp = (cp << 4) | (uint32_t)v;
                if (cp > 0x10FFFFu) return false;
            }
        } else {
            if (len > 9) return false; /* up to 8 digits per upstream */
            cp = 0;
            for (size_t i = 1; i < len; ++i) {
                if (name[i] < '0' || name[i] > '9') return false;
                cp = cp * 10u + (uint32_t)(name[i] - '0');
                if (cp > 0x10FFFFu) return false;
            }
        }
        if (!mdit_is_valid_entity_code(cp)) return false;
        return mdit_emit_utf8(cp, out);
    }
    /* Named entity. */
    const char *value = NULL;
    size_t      value_len = 0;
    if (!mdit_entity_lookup(name, len, &value, &value_len)) return false;
    return mdit_buf_append(out, value, value_len);
}

bool mdit_unescape_all(mdit_str input, mdit_buf *out)
{
    const char *s = input.data;
    size_t      n = input.len;

    /* Fast path: no anchor characters. */
    if (memchr(s, '\\', n) == NULL && memchr(s, '&', n) == NULL) {
        return mdit_buf_append(out, s, n);
    }

    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\' && i + 1 < n &&
            is_escapable((unsigned char)s[i + 1])) {
            if (!mdit_buf_append_byte(out, s[i + 1])) return false;
            i += 2;
            continue;
        }
        if (c == '&' && i + 1 < n &&
            entity_name_lead((unsigned char)s[i + 1])) {
            /* Scan to ';'. */
            size_t j = i + 2;
            size_t cap = (n - j > 31) ? 31 : (n - j); /* {1,31} per upstream */
            size_t scan_end = j + cap;
            while (j < scan_end &&
                   entity_name_tail((unsigned char)s[j])) {
                ++j;
            }
            /* Need a trailing ';' and at least 2 characters total
             * after '&' (i.e. lead + ';'). The upstream regex is
             * [a-z#][a-z0-9]{1,31}; → between 3 and 33 chars total. */
            size_t name_len = j - (i + 1);
            if (name_len >= 2 && name_len <= 32 &&
                j < n && s[j] == ';') {
                /* Try to resolve. If it lands, advance past the ';'. */
                size_t pre_len = mdit_buf_len(out);
                if (resolve_entity(s + i + 1, name_len, out)) {
                    i = j + 1;
                    continue;
                }
                /* Roll back any partial output (resolve_entity may
                 * have appended on failure of e.g. emit_utf8 mid-way).
                 * Currently resolve_entity only appends on full
                 * success, so this is defensive. */
                if (mdit_buf_len(out) != pre_len) {
                    /* truncate */
                    out->len = pre_len;
                    if (out->data) out->data[out->len] = '\0';
                }
            }
            /* Fall through: emit '&' verbatim and advance one byte. */
        }
        if (!mdit_buf_append_byte(out, (char)c)) return false;
        ++i;
    }
    return true;
}
