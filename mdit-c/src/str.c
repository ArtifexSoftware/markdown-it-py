/*
 * str.c — UTF-8 helpers and ASCII classifiers.
 *
 * The decoder is hand-rolled rather than table-driven for two reasons:
 * (a) zero static data at this layer keeps the .c small and easy to
 * audit; (b) the code is short enough that the compiler can inline
 * away the work in tight scanner loops. The shape closely follows
 * Bjoern Hoehrmann's well-known UTF-8 DFA in spirit but stays in
 * branchy form for readability.
 */
#include "str.h"

#include <string.h>

/* ---------------------------------------------------------------------
 * mdit_str — equality
 * ------------------------------------------------------------------- */
bool mdit_str_eq(mdit_str a, mdit_str b)
{
    if (a.len != b.len) return false;
    if (a.len == 0) return true;
    return memcmp(a.data, b.data, a.len) == 0;
}

bool mdit_str_eq_z(mdit_str a, const char *cstr)
{
    if (cstr == NULL) return a.data == NULL && a.len == 0;
    size_t n = strlen(cstr);
    if (a.len != n) return false;
    if (n == 0) return true;
    return memcmp(a.data, cstr, n) == 0;
}

bool mdit_str_eq_ci(mdit_str a, const char *cstr)
{
    if (cstr == NULL) return a.data == NULL && a.len == 0;
    size_t n = strlen(cstr);
    if (a.len != n) return false;
    for (size_t i = 0; i < n; ++i) {
        unsigned char x = (unsigned char)a.data[i];
        unsigned char y = (unsigned char)cstr[i];
        if (x >= 'A' && x <= 'Z') x = (unsigned char)(x + 0x20);
        if (y >= 'A' && y <= 'Z') y = (unsigned char)(y + 0x20);
        if (x != y) return false;
    }
    return true;
}

/* ---------------------------------------------------------------------
 * UTF-8 decode
 * ------------------------------------------------------------------- */
static size_t decode_one(const char *p, size_t len, uint32_t *out_cp,
                         bool *out_malformed)
{
    *out_malformed = false;
    if (len == 0) {
        *out_cp = MDIT_REPLACEMENT_CHAR;
        *out_malformed = true;
        return 0;
    }
    const unsigned char *u = (const unsigned char *)p;
    unsigned char c0 = u[0];

    if (c0 < 0x80) {
        *out_cp = c0;
        return 1;
    }

    /* Continuation bytes never start a sequence. */
    if (c0 < 0xC2 || c0 > 0xF4) {
        *out_cp = MDIT_REPLACEMENT_CHAR;
        *out_malformed = true;
        return 1;
    }

    size_t want;
    uint32_t cp;
    uint32_t min_cp;
    if (c0 < 0xE0) {                     /* 110xxxxx 10xxxxxx */
        want = 2;
        cp = c0 & 0x1Fu;
        min_cp = 0x80;
    } else if (c0 < 0xF0) {              /* 1110xxxx 10xxxxxx 10xxxxxx */
        want = 3;
        cp = c0 & 0x0Fu;
        min_cp = 0x800;
    } else {                             /* 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx */
        want = 4;
        cp = c0 & 0x07u;
        min_cp = 0x10000;
    }

    if (len < want) {
        *out_cp = MDIT_REPLACEMENT_CHAR;
        *out_malformed = true;
        return 1;
    }

    for (size_t i = 1; i < want; ++i) {
        unsigned char ci = u[i];
        if ((ci & 0xC0u) != 0x80u) {
            *out_cp = MDIT_REPLACEMENT_CHAR;
            *out_malformed = true;
            return 1;
        }
        cp = (cp << 6) | (ci & 0x3Fu);
    }

    if (cp < min_cp || cp > MDIT_CODEPOINT_MAX) {
        /* Overlong or out-of-range. */
        *out_cp = MDIT_REPLACEMENT_CHAR;
        *out_malformed = true;
        return 1;
    }
    if (cp >= 0xD800u && cp <= 0xDFFFu) {
        /* Surrogate halves are not valid UTF-8. */
        *out_cp = MDIT_REPLACEMENT_CHAR;
        *out_malformed = true;
        return 1;
    }

    *out_cp = cp;
    return want;
}

size_t mdit_decode(const char *data, size_t len, uint32_t *out_cp)
{
    bool malformed;
    size_t n = decode_one(data, len, out_cp, &malformed);
    return n; /* on malformed input, n == 1 and *out_cp == U+FFFD */
}

size_t mdit_decode_strict(const char *data, size_t len, uint32_t *out_cp)
{
    bool malformed;
    size_t n = decode_one(data, len, out_cp, &malformed);
    if (malformed) return 0;
    return n;
}

size_t mdit_encode(uint32_t cp, char *out, size_t cap)
{
    if (out == NULL) return 0;
    if (cp >= 0xD800u && cp <= 0xDFFFu) return 0;
    if (cp > MDIT_CODEPOINT_MAX) return 0;

    if (cp < 0x80u) {
        if (cap < 1) return 0;
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        if (cap < 2) return 0;
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        if (cap < 3) return 0;
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    if (cap < 4) return 0;
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >>  6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

bool mdit_is_utf8(const char *data, size_t len)
{
    size_t i = 0;
    while (i < len) {
        uint32_t cp;
        size_t n = mdit_decode_strict(data + i, len - i, &cp);
        if (n == 0) return false;
        i += n;
    }
    return true;
}

size_t mdit_count_codepoints(const char *data, size_t len)
{
    size_t i = 0, n = 0;
    while (i < len) {
        uint32_t cp;
        size_t step = mdit_decode(data + i, len - i, &cp);
        if (step == 0) break;
        i += step;
        ++n;
    }
    return n;
}

/* ---------------------------------------------------------------------
 * CommonMark ASCII punctuation
 *
 * Spec: !"#$%&'()*+,-./:;<=>?@[\]^_`{|}~
 * That's a contiguous-ish set of ASCII characters; we use a lookup
 * table for branchless classification — important because
 * ``isWhiteSpace`` / ``isPunctChar`` are called on every byte the
 * inline scanner sees.
 * ------------------------------------------------------------------- */
static const uint8_t s_md_punct_table[128] = {
    /*0x00..0x1F*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /*0x10..0x1F*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* '!"#$%&\'()*+,-./' */
    /*0x20*/       0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    /* '0123456789:;<=>?' */
    /*0x30*/       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1,
    /* '@ABCDEFGHIJKLMNO' */
    /*0x40*/       1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 'PQRSTUVWXYZ[\\]^_' */
    /*0x50*/       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1,
    /* '`abcdefghijklmno' */
    /*0x60*/       1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 'pqrstuvwxyz{|}~ ' */
    /*0x70*/       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0
};

bool mdit_is_md_ascii_punct(uint32_t c)
{
    return c < 128 && s_md_punct_table[c] != 0;
}
