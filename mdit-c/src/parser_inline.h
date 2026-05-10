/*
 * parser_inline.h — inline tokenizer driver.
 *
 * The inline parser owns two rulers: ``ruler`` for the per-character
 * matching pass and ``ruler2`` for the post-processing pass (balance
 * pairs, emphasis postProcess, etc.). The text rule reacts to a
 * "terminator set" of ASCII characters; we represent it as a 256-byte
 * boolean lookup table since all default terminators are ASCII.
 *
 * Built-in rules registered at init: text (catch-all). Additional
 * rules will be added incrementally.
 */
#ifndef MDIT_SRC_PARSER_INLINE_H
#define MDIT_SRC_PARSER_INLINE_H

#include <stdbool.h>
#include <stddef.h>

#include "ruler.h"
#include "state.h"
#include "token.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*mdit_inline_rule_fn) (mdit_state_inline *state, bool silent);
typedef void (*mdit_inline_rule2_fn)(mdit_state_inline *state);

typedef struct mdit_parser_inline {
    mdit_ruler   *ruler;
    mdit_ruler   *ruler2;
    mdit_lib_ctx *lib;
    mdit_arena   *arena;

    /* Bitmap of ASCII characters that terminate the text rule. Bytes
     * >= 0x80 are never terminators (no Unicode-aware terminators are
     * used by upstream's default rule set). */
    bool        terminator_ascii[256];
} mdit_parser_inline;

bool mdit_parser_inline_init   (mdit_parser_inline *p, mdit_lib_ctx *lib,
                                mdit_arena *arena);
void mdit_parser_inline_destroy(mdit_parser_inline *p);

/* Add a single ASCII char to the terminator set. (Plugin hook.) */
void mdit_parser_inline_add_terminator(mdit_parser_inline *p, char ch);

/* Run the matching loop + post-processing. ``parent`` is the inline
 * token whose ``children`` array gets populated. */
bool mdit_parser_inline_parse(mdit_parser_inline *p,
                              mdit_str src,
                              struct mdit_md *md,
                              void *env,
                              mdit_token *parent);

/* Skip a single token by running every rule in silent mode. Mirrors
 * upstream `ParserInline.skipToken`. Mutates `state->pos` only. */
void mdit_parser_inline_skip_token(mdit_parser_inline *p,
                                   mdit_state_inline *state);

#ifdef __cplusplus
}
#endif

#endif /* MDIT_SRC_PARSER_INLINE_H */
