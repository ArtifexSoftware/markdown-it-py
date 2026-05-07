/*
 * parser_block.h — block-level tokenizer driver.
 *
 * Holds an ``mdit_ruler`` of block rules (signature
 * ``bool(mdit_state_block *)``). Each rule reads its input range and
 * silent flag from ``state->cur_*`` set by the dispatch loop.
 *
 * Built-in rules registered at init: paragraph (catch-all). Additional
 * rules will be added incrementally as Phase 2 progresses.
 */
#ifndef MDIT_SRC_PARSER_BLOCK_H
#define MDIT_SRC_PARSER_BLOCK_H

#include <stdbool.h>

#include "ruler.h"
#include "state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*mdit_block_rule_fn)(mdit_state_block *state);

typedef struct mdit_parser_block {
    mdit_ruler *ruler;
    mdit_arena *arena;     /* shared with the owning MarkdownIt */
} mdit_parser_block;

/* Lifecycle. ``arena`` must outlive the parser. */
bool mdit_parser_block_init   (mdit_parser_block *p, mdit_arena *arena);
void mdit_parser_block_destroy(mdit_parser_block *p);

/* Tokenize the input range. The state must already be initialized via
 * mdit_state_block_init. */
void mdit_parser_block_tokenize(mdit_parser_block *p,
                                mdit_state_block *state,
                                int32_t start_line,
                                int32_t end_line);

/* Top-level entry: build a StateBlock from src and run the ruler. */
bool mdit_parser_block_parse(mdit_parser_block *p,
                             mdit_str src,
                             struct mdit_md *md,
                             void *env,
                             mdit_vec_token *out_tokens);

#ifdef __cplusplus
}
#endif

#endif /* MDIT_SRC_PARSER_BLOCK_H */
