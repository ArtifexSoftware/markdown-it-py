/*
 * parse_link_label.h — scan a CommonMark link label `[ ... ]`.
 *
 * Direct port of `helpers/parse_link_label.py`. The function consumes
 * tokens via `mdit_parser_inline_skip_token` so nested inline content
 * (escapes, backticks, autolinks, code spans, etc.) is skipped over
 * without producing tokens. Returns the byte offset of the matching
 * `]` on success, or `-1` if no balanced label is found.
 *
 * Caller passes `start` = byte offset of the opening `[`. State is
 * restored before returning.
 */
#ifndef MDIT_SRC_PARSE_LINK_LABEL_H
#define MDIT_SRC_PARSE_LINK_LABEL_H

#include <stdbool.h>
#include <stddef.h>

#include "parser_inline.h"
#include "state.h"

#ifdef __cplusplus
extern "C" {
#endif

ptrdiff_t mdit_parse_link_label(mdit_parser_inline *p,
                                mdit_state_inline *state,
                                size_t start,
                                bool disable_nested);

#ifdef __cplusplus
}
#endif

#endif /* MDIT_SRC_PARSE_LINK_LABEL_H */
