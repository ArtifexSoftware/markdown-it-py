/*
 * state.c — parser state implementations.
 */
#include "state.h"

#include <string.h>

#include "lib_alloc.h"
#include "main.h"
#include "vec.h"

/* Per-line cache vec. The DECLARE lives in state.h so other modules
 * (parser_block, etc.) can use the same type. */
MDIT_VEC_DEFINE(int32, int32_t)
MDIT_VEC_DEFINE(delimiter, mdit_delimiter)

/* ---------------------------------------------------------------------
 * StateCore
 * ------------------------------------------------------------------- */
void mdit_state_core_init(mdit_state_core *s,
                          mdit_arena *arena,
                          mdit_str src,
                          struct mdit_md *md,
                          void *env,
                          mdit_vec_token *tokens)
{
    s->src        = src;
    s->md         = md;
    s->env        = env;
    s->tokens     = tokens;
    s->inlineMode = false;
    s->arena      = arena;
}

/* ---------------------------------------------------------------------
 * StateBlock
 * ------------------------------------------------------------------- */
static bool is_str_space(unsigned char c)
{
    /* Mirrors `isStrSpace` from upstream: space + tab. */
    return c == ' ' || c == '\t';
}

bool mdit_state_block_init(mdit_state_block *s,
                           mdit_arena *arena,
                           mdit_str src,
                           struct mdit_md *md,
                           void *env,
                           mdit_vec_token *tokens)
{
    memset(s, 0, sizeof *s);
    s->src     = src;
    s->md      = md;
    s->env     = env;
    s->tokens  = tokens;
    s->arena   = arena;
    s->ddIndent   = -1;
    s->listIndent = -1;
    s->parent_type = MDIT_STR_LIT("root");
    s->code_enabled = true; /* default; ParserBlock can flip after init */

    /* Build the per-line caches. We accumulate into typed vecs first
     * and then snapshot the data pointers + length into the state. */
    mdit_vec_int32 b, e, ts, sc, bsc;
    mdit_vec_int32_init(&b,   md->lib, arena);
    mdit_vec_int32_init(&e,   md->lib, arena);
    mdit_vec_int32_init(&ts,  md->lib, arena);
    mdit_vec_int32_init(&sc,  md->lib, arena);
    mdit_vec_int32_init(&bsc, md->lib, arena);

    bool indent_found = false;
    int32_t start = 0, indent = 0, offset = 0;
    int32_t length = (int32_t)src.len;
    int32_t pos;

    for (pos = 0; pos < length; ++pos) {
        unsigned char ch = (unsigned char)src.data[pos];
        if (!indent_found) {
            if (is_str_space(ch)) {
                ++indent;
                if (ch == '\t') offset += 4 - (offset % 4);
                else            ++offset;
                continue;
            }
            indent_found = true;
        }
        if (ch == '\n' || pos == length - 1) {
            int32_t end_pos = (ch == '\n') ? pos : pos + 1;
            (void)mdit_vec_int32_push(&b,   start);
            (void)mdit_vec_int32_push(&e,   end_pos);
            (void)mdit_vec_int32_push(&ts,  indent);
            (void)mdit_vec_int32_push(&sc,  offset);
            (void)mdit_vec_int32_push(&bsc, 0);
            indent_found = false;
            indent = 0;
            offset = 0;
            start = pos + 1;
        }
    }

    /* Trailing fake row to simplify bounds checks. */
    (void)mdit_vec_int32_push(&b,   length);
    (void)mdit_vec_int32_push(&e,   length);
    (void)mdit_vec_int32_push(&ts,  0);
    (void)mdit_vec_int32_push(&sc,  0);
    (void)mdit_vec_int32_push(&bsc, 0);

    s->bMarks  = b.data;
    s->eMarks  = e.data;
    s->tShift  = ts.data;
    s->sCount  = sc.data;
    s->bsCount = bsc.data;
    s->n_lines = (b.len > 0) ? b.len - 1 : 0; /* exclude fake */
    s->lineMax = (int32_t)s->n_lines;
    return true;
}

mdit_token *mdit_state_block_push(mdit_state_block *s,
                                  mdit_str type, mdit_str tag, int8_t nesting)
{
    mdit_token *t = mdit_vec_token_emplace(s->tokens);
    if (t == NULL) return NULL;
    mdit_token_init(t, s->md->lib, s->arena, type, tag, nesting);
    t->block = true;
    if (nesting < 0) --s->level;
    t->level = s->level;
    if (nesting > 0) ++s->level;
    return t;
}

bool mdit_state_block_is_empty(const mdit_state_block *s, int32_t line)
{
    if (line < 0 || (size_t)line >= s->n_lines + 1) return true;
    return (s->bMarks[line] + s->tShift[line]) >= s->eMarks[line];
}

int32_t mdit_state_block_skip_empty_lines(const mdit_state_block *s, int32_t from)
{
    while (from < s->lineMax) {
        if ((s->bMarks[from] + s->tShift[from]) < s->eMarks[from]) break;
        ++from;
    }
    return from;
}

int32_t mdit_state_block_skip_spaces(const mdit_state_block *s, int32_t pos)
{
    int32_t n = (int32_t)s->src.len;
    while (pos < n && is_str_space((unsigned char)s->src.data[pos])) ++pos;
    return pos;
}

int32_t mdit_state_block_skip_spaces_back(const mdit_state_block *s,
                                          int32_t pos, int32_t minimum)
{
    if (pos <= minimum) return pos;
    while (pos > minimum) {
        --pos;
        if (!is_str_space((unsigned char)s->src.data[pos])) return pos + 1;
    }
    return pos;
}

int32_t mdit_state_block_skip_chars(const mdit_state_block *s,
                                    int32_t pos, char ch)
{
    int32_t n = (int32_t)s->src.len;
    while (pos < n && s->src.data[pos] == ch) ++pos;
    return pos;
}

int32_t mdit_state_block_skip_chars_back(const mdit_state_block *s,
                                         int32_t pos, char ch, int32_t minimum)
{
    if (pos <= minimum) return pos;
    while (pos > minimum) {
        --pos;
        if (s->src.data[pos] != ch) return pos + 1;
    }
    return pos;
}

bool mdit_state_block_is_code_block(const mdit_state_block *s, int32_t line)
{
    return s->code_enabled && (s->sCount[line] - s->blkIndent) >= 4;
}

bool mdit_state_block_get_lines(const mdit_state_block *s,
                                int32_t begin, int32_t end,
                                int32_t indent, bool keep_last_lf,
                                mdit_buf *out)
{
    if (begin >= end) return true;
    int32_t line = begin;
    while (line < end) {
        int32_t lineIndent = 0;
        int32_t lineStart  = s->bMarks[line];
        int32_t first      = lineStart;
        int32_t last;
        if (line + 1 < end || keep_last_lf) {
            last = s->eMarks[line] + 1;
        } else {
            last = s->eMarks[line];
        }
        if (last > (int32_t)s->src.len) last = (int32_t)s->src.len;

        while (first < last && lineIndent < indent) {
            unsigned char ch = (unsigned char)s->src.data[first];
            if (is_str_space(ch)) {
                if (ch == '\t') {
                    lineIndent += 4 - ((lineIndent + s->bsCount[line]) % 4);
                } else {
                    ++lineIndent;
                }
            } else if (first - lineStart < s->tShift[line]) {
                ++lineIndent;
            } else {
                break;
            }
            ++first;
        }

        if (lineIndent > indent) {
            /* partially-expand tabs */
            for (int32_t i = 0; i < lineIndent - indent; ++i) {
                if (!mdit_buf_append_byte(out, ' ')) return false;
            }
        }
        if (last > first) {
            if (!mdit_buf_append(out, s->src.data + first,
                                 (size_t)(last - first))) return false;
        }
        ++line;
    }
    return true;
}

/* ---------------------------------------------------------------------
 * StateInline
 * ------------------------------------------------------------------- */
void mdit_state_inline_init(mdit_state_inline *s,
                            mdit_arena *arena,
                            mdit_str src,
                            struct mdit_md *md,
                            void *env,
                            mdit_token *parent)
{
    memset(s, 0, sizeof *s);
    s->src     = src;
    s->md      = md;
    s->env     = env;
    s->parent  = parent;
    s->arena   = arena;
    s->pos     = 0;
    s->pos_max = src.len;
    mdit_buf_init(&s->pending, md->lib);

    s->delimiters = (mdit_vec_delimiter *)
        mdit_arena_alloc(md->lib, arena, sizeof *s->delimiters);
    mdit_vec_delimiter_init(s->delimiters, md->lib, arena);

    s->delim_stack       = NULL;
    s->delim_stack_len   = 0;
    s->delim_stack_cap   = 0;
    s->tokens_meta       = NULL;
    s->tokens_meta_len   = 0;
    s->tokens_meta_cap   = 0;

    s->cache     = NULL;
    s->cache_len = 0;
}

void mdit_state_inline_destroy(mdit_state_inline *s)
{
    mdit_buf_destroy(&s->pending);
    /* `delimiters` and any pool entries live in the arena -- nothing
     * to free here other than the malloc-backed scaffolding. */
    mdit_lib_ctx *lib = (s->md != NULL) ? s->md->lib : NULL;
    mdit_lib_free_bytes(lib, s->delim_stack);
    mdit_lib_free_bytes(lib, s->tokens_meta);
    mdit_lib_free_bytes(lib, s->cache);
    s->delim_stack    = NULL;
    s->tokens_meta    = NULL;
    s->cache          = NULL;
    s->delim_stack_len = s->delim_stack_cap = 0;
    s->tokens_meta_len = s->tokens_meta_cap = 0;
    s->cache_len = 0;
}

static bool stk_push_delim(mdit_state_inline *s, mdit_vec_delimiter *p)
{
    if (s->delim_stack_len == s->delim_stack_cap) {
        size_t new_cap = (s->delim_stack_cap == 0) ? 4 : s->delim_stack_cap * 2;
        mdit_vec_delimiter **tmp = (mdit_vec_delimiter **)mdit_lib_realloc_bytes(
            s->md->lib, s->delim_stack, new_cap * sizeof *tmp);
        if (tmp == NULL) return false;
        s->delim_stack = tmp;
        s->delim_stack_cap = new_cap;
    }
    s->delim_stack[s->delim_stack_len++] = p;
    return true;
}

static mdit_vec_delimiter *stk_pop_delim(mdit_state_inline *s)
{
    if (s->delim_stack_len == 0) return NULL;
    return s->delim_stack[--s->delim_stack_len];
}

static bool tm_push(mdit_state_inline *s, mdit_vec_delimiter *p)
{
    if (s->tokens_meta_len == s->tokens_meta_cap) {
        size_t new_cap = (s->tokens_meta_cap == 0) ? 16 : s->tokens_meta_cap * 2;
        mdit_vec_delimiter **tmp = (mdit_vec_delimiter **)mdit_lib_realloc_bytes(
            s->md->lib, s->tokens_meta, new_cap * sizeof *tmp);
        if (tmp == NULL) return false;
        s->tokens_meta = tmp;
        s->tokens_meta_cap = new_cap;
    }
    s->tokens_meta[s->tokens_meta_len++] = p;
    return true;
}

bool mdit_state_inline_append_pending(mdit_state_inline *s,
                                      const char *bytes, size_t n)
{
    return mdit_buf_append(&s->pending, bytes, n);
}

mdit_token *mdit_state_inline_push_pending(mdit_state_inline *s)
{
    mdit_token *t = mdit_token_push_child(s->parent,
                                          MDIT_STR_LIT("text"),
                                          MDIT_STR_LIT(""), 0);
    if (t == NULL) return NULL;
    /* Copy pending bytes into the arena so the token retains a stable
     * view after the pending buffer is reset. */
    if (s->pending.len > 0) {
        char *buf = (char *)mdit_arena_alloc(s->md->lib, s->arena, s->pending.len);
        memcpy(buf, s->pending.data, s->pending.len);
        t->content.data = buf;
        t->content.len  = s->pending.len;
    }
    t->level = s->pendingLevel;
    mdit_buf_reset(&s->pending);
    /* Pending text tokens are non-opening; record a NULL slot in
     * tokens_meta to keep it parallel to parent->children. */
    (void)tm_push(s, NULL);
    return t;
}

mdit_token *mdit_state_inline_push(mdit_state_inline *s,
                                   mdit_str type, mdit_str tag, int8_t nesting)
{
    if (s->pending.len > 0) {
        (void)mdit_state_inline_push_pending(s);
    }
    /* Closing tag: pop the current scope. */
    if (nesting < 0) {
        --s->level;
        mdit_vec_delimiter *prev = stk_pop_delim(s);
        if (prev != NULL) s->delimiters = prev;
    }
    mdit_token *t = mdit_token_push_child(s->parent, type, tag, nesting);
    if (t == NULL) return NULL;
    t->level = s->level;

    mdit_vec_delimiter *meta_for_token = NULL;

    /* Opening tag: push current scope, allocate a fresh delimiter
     * list, attach to this token via tokens_meta. */
    if (nesting > 0) {
        ++s->level;
        (void)stk_push_delim(s, s->delimiters);
        mdit_vec_delimiter *fresh = (mdit_vec_delimiter *)
            mdit_arena_alloc(s->md->lib, s->arena, sizeof *fresh);
        if (fresh == NULL) return NULL;
        mdit_vec_delimiter_init(fresh, s->md->lib, s->arena);
        s->delimiters = fresh;
        meta_for_token = fresh;
    }

    s->pendingLevel = s->level;

    (void)tm_push(s, meta_for_token);

    return t;
}
