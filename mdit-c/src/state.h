/*
 * state.h — parser state types.
 *
 * Three states mirror their Python counterparts:
 *
 *   ``mdit_state_core``    — owned by ParserCore.process(); carries
 *                            the (mutable) source string, env, and
 *                            output token vec across the core chain.
 *
 *   ``mdit_state_block``   — owned by ParserBlock.tokenize(); carries
 *                            the per-line caches (bMarks/eMarks/
 *                            tShift/sCount/bsCount), block-rule
 *                            scratch fields (blkIndent / line /
 *                            lineMax / parent_type / level / tight)
 *                            and the rule-call args (start_line,
 *                            end_line, silent) so the rule signature
 *                            stays a single-pointer C function call.
 *
 *   ``mdit_state_inline``  — owned by ParserInline.tokenize(); carries
 *                            the byte cursor (pos, pos_max), pending
 *                            text accumulator, current level, and the
 *                            output token list.
 *
 * All allocations come from the parser's arena, including the per-line
 * cache arrays. State objects themselves live on the stack of the
 * caller (typically MarkdownIt.parse).
 */
#ifndef MDIT_SRC_STATE_H
#define MDIT_SRC_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "json.h"
#include "str.h"
#include "token.h"
#include "vec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward — defined in main.h. */
struct mdit_md;

/* Typed int32 vector used by the per-line caches and by block rules
 * (blockquote restore-snapshots etc.). */
MDIT_VEC_DECLARE(int32, int32_t)

typedef struct mdit_delimiter {
    int32_t marker;
    int32_t length;
    int32_t token;
    int32_t end;
    bool    open;
    bool    close;
} mdit_delimiter;

MDIT_VEC_DECLARE(delimiter, mdit_delimiter)

/* ---------------------------------------------------------------------
 * StateCore
 * ------------------------------------------------------------------- */
typedef struct mdit_state_core {
    mdit_str         src;
    struct mdit_md  *md;
    void            *env;
    mdit_vec_token  *tokens;     /* borrowed pointer to MarkdownIt's vec */
    bool             inlineMode;
    mdit_arena      *arena;      /* backing arena for any new allocations */
} mdit_state_core;

void mdit_state_core_init(mdit_state_core *s,
                          mdit_arena *arena,
                          mdit_str src,
                          struct mdit_md *md,
                          void *env,
                          mdit_vec_token *tokens);

/* ---------------------------------------------------------------------
 * StateBlock
 * ------------------------------------------------------------------- */
typedef struct mdit_state_block {
    mdit_str         src;
    struct mdit_md  *md;
    void            *env;
    mdit_vec_token  *tokens;
    mdit_arena      *arena;

    /* Per-line caches. Length is `n_lines + 1` (the trailing fake line
     * at the end matches upstream). */
    int32_t         *bMarks;
    int32_t         *eMarks;
    int32_t         *tShift;
    int32_t         *sCount;
    int32_t         *bsCount;
    size_t           n_lines;     /* line count not counting the fake row */

    int32_t          blkIndent;
    int32_t          line;
    int32_t          lineMax;
    bool             tight;
    int32_t          ddIndent;
    int32_t          listIndent;
    mdit_str         parent_type; /* "root" / "paragraph" / "blockquote" / ... */
    int32_t          level;

    /* Block rules read these to find their input range / silent flag.
     * The ParserBlock dispatch updates them before each rule call. */
    int32_t          cur_start_line;
    int32_t          cur_end_line;
    bool             cur_silent;

    /* Cached: is the `code` block rule currently enabled? Used by
     * is_code_block() to short-circuit the indented-code check. */
    bool             code_enabled;
} mdit_state_block;

bool mdit_state_block_init(mdit_state_block *s,
                           mdit_arena *arena,
                           mdit_str src,
                           struct mdit_md *md,
                           void *env,
                           mdit_vec_token *tokens);

/* Push a new block-level token. Sets `block = true`, manages level. */
mdit_token *mdit_state_block_push(mdit_state_block *s,
                                  mdit_str type, mdit_str tag, int8_t nesting);

/* Line introspection helpers — direct ports of Python methods. */
bool   mdit_state_block_is_empty       (const mdit_state_block *s, int32_t line);
int32_t mdit_state_block_skip_empty_lines(const mdit_state_block *s, int32_t from);
int32_t mdit_state_block_skip_spaces   (const mdit_state_block *s, int32_t pos);
int32_t mdit_state_block_skip_spaces_back(const mdit_state_block *s,
                                         int32_t pos, int32_t minimum);
int32_t mdit_state_block_skip_chars    (const mdit_state_block *s,
                                         int32_t pos, char ch);
int32_t mdit_state_block_skip_chars_back(const mdit_state_block *s,
                                         int32_t pos, char ch, int32_t minimum);
bool   mdit_state_block_is_code_block  (const mdit_state_block *s, int32_t line);

/* Cut lines [begin, end) out of the source, stripping `indent` spaces
 * worth of leading whitespace from each line. Result is appended to
 * ``out``; ``keep_last_lf`` controls whether the final '\n' is kept. */
bool mdit_state_block_get_lines(const mdit_state_block *s,
                                int32_t begin, int32_t end,
                                int32_t indent, bool keep_last_lf,
                                mdit_buf *out);

/* ---------------------------------------------------------------------
 * StateInline
 *
 * Tokens always flow into a parent token's `children` array (the inline
 * parser is only ever called with an inline token as the destination).
 * That mirrors `state.md.inline.parse(content, md, env, token.children)`
 * upstream, while staying aligned with how `mdit_token` already stores
 * its children as a flat arena-allocated array.
 * ------------------------------------------------------------------- */
typedef struct mdit_state_inline {
    mdit_str         src;
    struct mdit_md  *md;
    void            *env;
    mdit_token      *parent;     /* destination — children appended here */
    mdit_arena      *arena;

    size_t           pos;
    size_t           pos_max;
    int32_t          level;
    int32_t          pendingLevel;

    mdit_buf         pending;    /* pending text accumulator */

    /* Currently active delimiter list. Mirrors upstream's
     * `state.delimiters` -- swapped on `push()` when an opening or
     * closing token enters/leaves a nested scope (link / image label).
     * Points into a heap-managed pool; never NULL after init. */
    mdit_vec_delimiter *delimiters;

    /* Stack of saved delimiter-list pointers. `push()` with nesting>0
     * pushes the current `delimiters` here; nesting<0 pops. Allocated
     * lazily on first nested scope. */
    mdit_vec_delimiter **delim_stack;
    size_t              delim_stack_len;
    size_t              delim_stack_cap;

    /* Parallel array to `parent->children`. NULL for non-opening
     * tokens; for opening tokens, holds the delimiter list scoped to
     * that token's inner content. balance_pairs / emphasis postProcess
     * walk this in addition to `state->delimiters`. */
    mdit_vec_delimiter **tokens_meta;
    size_t              tokens_meta_len;
    size_t              tokens_meta_cap;

    /* skipToken cache. Indexed by byte offset into `src`; entries are
     * initialised to -1 and filled in lazily by skipToken. Mirrors
     * upstream's per-state `cache` dict. */
    int32_t         *cache;
    size_t           cache_len;

    /* Counter used to disable inline linkify-it execution inside
     * markdown links and `<a>` HTML tags. Mirrors upstream
     * `state.linkLevel`. */
    int32_t          link_level;

    /* Inline rules need to know if they're running in silent (skipToken)
     * mode. */
    bool             cur_silent;
} mdit_state_inline;

void mdit_state_inline_init(mdit_state_inline *s,
                            mdit_arena *arena,
                            mdit_str src,
                            struct mdit_md *md,
                            void *env,
                            mdit_token *parent);
void mdit_state_inline_destroy(mdit_state_inline *s);

/* Flush the pending text buffer as a single ``text`` token. */
mdit_token *mdit_state_inline_push_pending(mdit_state_inline *s);

/* Push a new inline token; flushes any pending text first. */
mdit_token *mdit_state_inline_push(mdit_state_inline *s,
                                   mdit_str type, mdit_str tag, int8_t nesting);

/* Append `n` bytes (UTF-8) of source to the pending accumulator. */
bool mdit_state_inline_append_pending(mdit_state_inline *s,
                                      const char *bytes, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* MDIT_SRC_STATE_H */
