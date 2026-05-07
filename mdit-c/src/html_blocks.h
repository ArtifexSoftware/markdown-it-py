/*
 * html_blocks.h — list of HTML block-level tag names per CommonMark.
 *
 * Mirrors `markdown_it.common.html_blocks.block_names`. Used by
 * `block_html_block` to decide whether an opening tag triggers HTML
 * sequence 6.
 */
#ifndef MDIT_SRC_HTML_BLOCKS_H
#define MDIT_SRC_HTML_BLOCKS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Case-insensitive lookup against the CommonMark `block_names` set.
 * `s`/`len` is an ASCII tag name (no leading `<` or `</`). */
bool mdit_html_is_block_name(const char *s, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* MDIT_SRC_HTML_BLOCKS_H */
