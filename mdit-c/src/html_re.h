/*
 * html_re.h — hand-coded equivalents of the CommonMark HTML regexes.
 *
 * Mirrors `markdown_it.common.html_re`. Phase 2 needs only the
 * open-or-close-tag matcher anchored at start of input (sequence 7 of
 * the html_block rule). The full HTML_TAG_RE — including comments,
 * CDATA, processing instructions, and declarations — will land later
 * when the inline `html_inline` rule is ported.
 */
#ifndef MDIT_SRC_HTML_RE_H
#define MDIT_SRC_HTML_RE_H

#include <stdbool.h>
#include <stddef.h>

#include "str.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Match an HTML open or close tag at `s[0..]`. Returns 0 if `s` does
 * not start with one, else the number of bytes consumed.
 *
 * Equivalent to (anchored at start):
 *
 *   open_tag  = <[A-Za-z][A-Za-z0-9-]* attribute* \s* /? >
 *   close_tag = </[A-Za-z][A-Za-z0-9-]* \s* >
 *
 * `attribute = \s+ name (\s* = \s* value)?` with the usual three
 * value variants (unquoted / single-quoted / double-quoted). */
size_t mdit_html_match_open_close_tag(const char *s, size_t n);

/* Match any HTML tag form at `s[0..]`. Returns 0 if `s` does not start
 * with one, else the number of bytes consumed. Mirrors HTML_TAG_RE
 * from upstream:
 *
 *   open_tag | close_tag | comment | processing | declaration | cdata
 */
size_t mdit_html_match_tag(const char *s, size_t n);

/* Recognise `<a` open tag (case-insensitive, as used by the linkify
 * core rule's HTML-link-suppression state machine). Mirrors
 * `LINK_OPEN_RE = ^<a[>\s]` from `markdown_it.common.utils`. */
bool mdit_html_is_link_open (mdit_str s);

/* Recognise `</a>` close tag (case-insensitive). Mirrors
 * `LINK_CLOSE_RE = ^</a\s*>`. */
bool mdit_html_is_link_close(mdit_str s);

#ifdef __cplusplus
}
#endif

#endif /* MDIT_SRC_HTML_RE_H */
