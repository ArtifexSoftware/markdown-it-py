/*
 * html_blocks.c — CommonMark HTML block-tag set.
 *
 * The list comes verbatim from `markdown_it.common.html_blocks.py`
 * (CommonMark 0.31.2 + a few HTML5 names). We sort it once and use
 * binary search; the table is small enough that a perfect hash
 * wouldn't pay for itself.
 */
#include "html_blocks.h"

#include <stddef.h>
#include <string.h>

/* Sorted, lower-case. Must stay sorted for the binary search. */
static const char *const HTML_BLOCK_NAMES[] = {
    "address", "article", "aside",
    "base", "basefont", "blockquote", "body",
    "caption", "center", "col", "colgroup",
    "dd", "details", "dialog", "dir", "div", "dl", "dt",
    "fieldset", "figcaption", "figure", "footer", "form", "frame", "frameset",
    "h1", "h2", "h3", "h4", "h5", "h6",
    "head", "header", "hr", "html",
    "iframe",
    "legend", "li", "link",
    "main", "menu", "menuitem",
    "nav", "noframes",
    "ol", "optgroup", "option",
    "p", "param",
    "search", "section", "summary",
    "table", "tbody", "td", "tfoot", "th", "thead", "title", "tr", "track",
    "ul",
};
static const size_t HTML_BLOCK_NAMES_LEN =
    sizeof HTML_BLOCK_NAMES / sizeof HTML_BLOCK_NAMES[0];

static int icmp_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
        /* `b` is the table entry; already lowercase. */
        if (ca != cb) return (int)ca - (int)cb;
    }
    return 0;
}

bool mdit_html_is_block_name(const char *s, size_t len)
{
    if (len == 0 || len > 16) return false; /* longest is 'figcaption' = 10 */
    size_t lo = 0, hi = HTML_BLOCK_NAMES_LEN;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const char *cand = HTML_BLOCK_NAMES[mid];
        size_t cand_len = strlen(cand);
        size_t n = (len < cand_len) ? len : cand_len;
        int c = icmp_n(s, cand, n);
        if (c == 0) {
            if (len == cand_len) return true;
            c = (len < cand_len) ? -1 : 1;
        }
        if (c < 0) hi = mid;
        else       lo = mid + 1;
    }
    return false;
}
