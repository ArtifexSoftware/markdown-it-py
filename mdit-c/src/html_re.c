/*
 * html_re.c — open/close tag scanner.
 *
 * One-pass scanner mirroring HTML_OPEN_CLOSE_TAG_RE from upstream.
 * No backtracking is needed: the grammar is unambiguous once the
 * leading `<` (and optional `/`) has been seen. We match attributes
 * and whitespace greedily, fail fast, and return the byte length of
 * the consumed tag (or 0 if the input doesn't start with one).
 */
#include "html_re.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static bool is_alpha(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }

static bool is_tag_name_cont(unsigned char c)
{
    return is_alpha(c) || is_digit(c) || c == '-';
}

static bool is_attr_name_lead(unsigned char c)
{
    return is_alpha(c) || c == '_' || c == ':';
}

static bool is_attr_name_cont(unsigned char c)
{
    return is_alpha(c) || is_digit(c) || c == ':' ||
           c == '.' || c == '_' || c == '-';
}

static bool is_ws(unsigned char c)
{
    return c == ' ' || (c >= 0x09 && c <= 0x0D);
}

static bool is_unquoted(unsigned char c)
{
    /* [^"'=<>`\x00-\x20] */
    if (c <= 0x20) return false;
    switch (c) {
        case '"': case '\'': case '=': case '<': case '>': case '`':
            return false;
        default: return true;
    }
}

/* Consume one attribute starting at s[off]. Requires at least one
 * leading whitespace byte. Returns the new offset, or 0 if no
 * attribute matches at this position. */
static size_t match_attribute(const char *s, size_t n, size_t off)
{
    size_t p = off;
    if (p >= n || !is_ws((unsigned char)s[p])) return 0;
    while (p < n && is_ws((unsigned char)s[p])) ++p;
    if (p >= n || !is_attr_name_lead((unsigned char)s[p])) return 0;
    ++p;
    while (p < n && is_attr_name_cont((unsigned char)s[p])) ++p;

    /* Optional `\s* = \s* value`. */
    size_t save = p;
    while (p < n && is_ws((unsigned char)s[p])) ++p;
    if (p >= n || s[p] != '=') return save;
    ++p;
    while (p < n && is_ws((unsigned char)s[p])) ++p;
    if (p >= n) return 0;

    unsigned char q = (unsigned char)s[p];
    if (q == '"' || q == '\'') {
        ++p;
        while (p < n && (unsigned char)s[p] != q) ++p;
        if (p >= n) return 0;
        ++p;
        return p;
    }
    /* Unquoted: at least one valid char. */
    if (!is_unquoted(q)) return 0;
    while (p < n && is_unquoted((unsigned char)s[p])) ++p;
    return p;
}

/* `<!---?>` short or `<!--(?:[^-]|-[^-]|--[^>])*-->` long. */
static size_t match_comment(const char *s, size_t n)
{
    /* Already validated `<!--`. */
    size_t p = 4;
    if (p < n && s[p] == '>') return p + 1;             /* <!--> */
    if (p + 1 < n && s[p] == '-' && s[p + 1] == '>') return p + 2; /* <!---> */
    while (p + 2 < n) {
        if (s[p] == '-' && s[p + 1] == '-') {
            if (s[p + 2] == '>') return p + 3;
            /* Regex branch `--[^>]`: double hyphen is allowed inside
             * an HTML comment as long as it is not followed by `>`.
             * Consume all three bytes and keep scanning. */
            p += 3;
            continue;
        }
        ++p;
    }
    return 0;
}

/* `<\?[\s\S]*?\?>` */
static size_t match_processing(const char *s, size_t n)
{
    size_t p = 2;
    while (p + 1 < n) {
        if (s[p] == '?' && s[p + 1] == '>') return p + 2;
        ++p;
    }
    return 0;
}

/* `<![A-Za-z][^>]*>` */
static size_t match_declaration(const char *s, size_t n)
{
    if (n < 3) return 0;
    unsigned char c = (unsigned char)s[2];
    if (!is_alpha(c)) return 0;
    size_t p = 3;
    while (p < n && s[p] != '>') ++p;
    if (p >= n) return 0;
    return p + 1;
}

/* `<!\[CDATA\[[\s\S]*?\]\]>` */
static size_t match_cdata(const char *s, size_t n)
{
    /* Already validated `<![CDATA[`. */
    size_t p = 9;
    while (p + 2 < n) {
        if (s[p] == ']' && s[p + 1] == ']' && s[p + 2] == '>') return p + 3;
        ++p;
    }
    return 0;
}

size_t mdit_html_match_tag(const char *s, size_t n)
{
    if (n < 3 || s[0] != '<') return 0;
    unsigned char c1 = (unsigned char)s[1];
    if (c1 == '!') {
        if (n >= 4 && s[2] == '-' && s[3] == '-') return match_comment(s, n);
        if (n >= 9 && s[2] == '[' && memcmp(s + 3, "CDATA[", 6) == 0) {
            return match_cdata(s, n);
        }
        return match_declaration(s, n);
    }
    if (c1 == '?') return match_processing(s, n);
    return mdit_html_match_open_close_tag(s, n);
}

size_t mdit_html_match_open_close_tag(const char *s, size_t n)
{
    if (n < 3 || s[0] != '<') return 0;
    size_t p = 1;
    bool is_close = false;
    if (s[p] == '/') {
        is_close = true;
        ++p;
    }
    if (p >= n || !is_alpha((unsigned char)s[p])) return 0;
    ++p;
    while (p < n && is_tag_name_cont((unsigned char)s[p])) ++p;

    if (is_close) {
        while (p < n && is_ws((unsigned char)s[p])) ++p;
        if (p >= n || s[p] != '>') return 0;
        return p + 1;
    }

    /* Attributes (zero or more). */
    while (p < n) {
        size_t after_attr = match_attribute(s, n, p);
        if (after_attr == 0 || after_attr == p) break;
        p = after_attr;
    }
    while (p < n && is_ws((unsigned char)s[p])) ++p;
    if (p < n && s[p] == '/') ++p;
    if (p >= n || s[p] != '>') return 0;
    return p + 1;
}

/* `^<a[>\s]` — case-insensitive on the `a`. */
bool mdit_html_is_link_open(mdit_str s)
{
    if (s.len < 3) return false;
    if (s.data[0] != '<') return false;
    unsigned char c = (unsigned char)s.data[1];
    if (c != 'a' && c != 'A') return false;
    unsigned char d = (unsigned char)s.data[2];
    return d == '>' || is_ws(d);
}

/* `^</a\s*>` — case-insensitive on the `a`. */
bool mdit_html_is_link_close(mdit_str s)
{
    if (s.len < 4) return false;
    if (s.data[0] != '<' || s.data[1] != '/') return false;
    unsigned char c = (unsigned char)s.data[2];
    if (c != 'a' && c != 'A') return false;
    size_t i = 3;
    while (i < s.len && is_ws((unsigned char)s.data[i])) ++i;
    return i < s.len && s.data[i] == '>';
}
