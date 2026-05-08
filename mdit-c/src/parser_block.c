/*
 * parser_block.c — block-level tokenizer driver + built-in rules.
 *
 * The ruler stores generic ``(state, user)`` callbacks; block rules
 * have signature ``bool(mdit_state_block *)``. We exploit C11
 * §6.3.2.3p8: pointer-to-function may be converted to another
 * pointer-to-function and back, and a call through the original type
 * is well-defined. So we cast the typed rule fn to mdit_rule_fn at
 * registration and back to the typed signature at dispatch.
 */
#include "parser_block.h"

#include <string.h>

#include "env.h"
#include "html_blocks.h"
#include "html_re.h"
#include "link_helpers.h"
#include "main.h"
#include "normalize_url.h"

/* ---------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------- */
static bool char_is_space(unsigned char c) { return c == ' ' || c == '\t'; }

/* GitHub-style alert kinds recognised by the `alerts` extension —
 * mirrors `_ALERT_TYPES` in markdown_it/rules_block/blockquote.py.
 * Order matters only insofar as the labels are checked left-to-right;
 * upstream uses a set so order is irrelevant — we match the upstream
 * order for readability. */
static const struct {
    const char *name;
    size_t      len;
    const char *capitalised; /* `name.capitalize()` (Python semantics) */
} g_alert_kinds[] = {
    { "NOTE",      4, "Note"      },
    { "TIP",       3, "Tip"       },
    { "IMPORTANT", 9, "Important" },
    { "WARNING",   7, "Warning"   },
    { "CAUTION",   7, "Caution"   },
};

/* Detect `[!KIND]` on a single source line *line_start..line_end*.
 * Returns the matching index in `g_alert_kinds` (>= 0), or -1 for no
 * match. Mirrors `_detect_alert` in
 * markdown_it/rules_block/blockquote.py: upstream trims trailing
 * whitespace, then requires an opening `[!`, a closing `]`, and a body
 * that *case-insensitively* matches one of the recognised kinds. */
static int detect_alert(const char *src, int32_t pos, int32_t maximum)
{
    while (maximum > pos &&
           (src[maximum - 1] == ' ' || src[maximum - 1] == '\t')) {
        --maximum;
    }
    if (maximum - pos < 4) return -1;
    if (src[pos] != '[' || src[pos + 1] != '!') return -1;
    if (src[maximum - 1] != ']') return -1;
    const char *body     = src + pos + 2;
    int32_t     body_len = (maximum - 1) - (pos + 2);
    if (body_len <= 0) return -1;

    for (size_t i = 0; i < sizeof g_alert_kinds / sizeof *g_alert_kinds; ++i) {
        if ((int32_t)g_alert_kinds[i].len != body_len) continue;
        bool match = true;
        for (int32_t j = 0; j < body_len; ++j) {
            char a = body[j];
            char b = g_alert_kinds[i].name[j];
            /* ASCII upper-case fold; matches Python's str.upper() for
             * the ASCII letters that make up our kind names. */
            if (a >= 'a' && a <= 'z') a = (char)(a - 32);
            if (a != b) { match = false; break; }
        }
        if (match) return (int)i;
    }
    return -1;
}

/* GFM task checkbox detector — mirrors `_detect_task_checkbox` in
 * markdown_it/rules_block/list.py. Returns -1 for "no match", 0 for an
 * unchecked `[ ]`, or 1 for a checked `[x]`/`[X]`. The trailing
 * whitespace requirement (` ` or `\t`) is part of the contract — the
 * caller can safely advance by exactly 4 bytes when the result is
 * non-negative. */
static int detect_task_checkbox(const char *src, int32_t pos, int32_t maximum)
{
    if (pos + 4 > maximum) return -1;
    if (src[pos] != '[') return -1;
    char inner = src[pos + 1];
    if (src[pos + 2] != ']') return -1;
    int checked;
    if (inner == ' ') {
        checked = 0;
    } else if (inner == 'x' || inner == 'X') {
        checked = 1;
    } else {
        return -1;
    }
    char after = src[pos + 3];
    if (after != ' ' && after != '\t') return -1;
    return checked;
}

/* Allocate a fresh copy of `data[0..n)` in the arena. */
static mdit_str arena_copy_str(mdit_arena *a, const char *data, size_t n)
{
    if (n == 0) return MDIT_STR_LIT("");
    char *buf = (char *)mdit_arena_alloc(a, n);
    memcpy(buf, data, n);
    return (mdit_str){ buf, n };
}

/* Strip leading/trailing ASCII whitespace ([ \t\r\n\f\v]) from `s`,
 * returning a borrowed sub-view (no copy). */
static mdit_str str_strip(mdit_str s)
{
    size_t lo = 0, hi = s.len;
    while (lo < hi) {
        unsigned char c = (unsigned char)s.data[lo];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
            c == '\v' || c == '\f') ++lo;
        else break;
    }
    while (hi > lo) {
        unsigned char c = (unsigned char)s.data[hi - 1];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
            c == '\v' || c == '\f') --hi;
        else break;
    }
    return (mdit_str){ s.data + lo, hi - lo };
}

static bool is_ref_space(unsigned char c) { return c == ' ' || c == '\t'; }

static bool append_reference_next_line(mdit_state_block *state,
                                       int32_t nextLine,
                                       mdit_buf *out)
{
    if (nextLine >= state->lineMax) return false;
    if (mdit_state_block_is_empty(state, nextLine)) return false;

    bool isContinuation = false;
    if (mdit_state_block_is_code_block(state, nextLine)) isContinuation = true;
    if (state->sCount[nextLine] < 0) isContinuation = true;

    if (!isContinuation) {
        size_t n_term = 0;
        const mdit_rule_entry *terminators =
            mdit_ruler_get_rules(state->md->block.ruler,
                                 MDIT_STR_LIT("reference"), &n_term);
        mdit_str old_parent = state->parent_type;
        int32_t old_line = state->line;
        int32_t old_lineMax = state->lineMax;
        state->parent_type = MDIT_STR_LIT("reference");

        bool terminate = false;
        for (size_t i = 0; i < n_term; ++i) {
            mdit_block_rule_fn fn = (mdit_block_rule_fn)terminators[i].fn;
            int32_t ssl = state->cur_start_line, sel = state->cur_end_line;
            bool    sst = state->cur_silent;
            state->cur_start_line = nextLine;
            state->cur_end_line   = state->lineMax;
            state->cur_silent     = true;
            bool matched = fn(state);
            state->cur_start_line = ssl;
            state->cur_end_line   = sel;
            state->cur_silent     = sst;
            if (matched) { terminate = true; break; }
        }
        state->parent_type = old_parent;
        state->line = old_line;
        state->lineMax = old_lineMax;
        if (terminate) return false;
    }

    int32_t pos = state->bMarks[nextLine] + state->tShift[nextLine];
    int32_t maximum = state->eMarks[nextLine] + 1;
    if (maximum > (int32_t)state->src.len) maximum = (int32_t)state->src.len;
    return mdit_buf_append(out, state->src.data + pos, (size_t)(maximum - pos));
}

/* ---------------------------------------------------------------------
 * Built-in rule: code (indented 4-space block).
 * ------------------------------------------------------------------- */
static bool block_code(mdit_state_block *state)
{
    int32_t startLine = state->cur_start_line;
    int32_t endLine   = state->cur_end_line;
    if (!mdit_state_block_is_code_block(state, startLine)) return false;

    int32_t last = startLine + 1;
    int32_t nextLine = startLine + 1;
    while (nextLine < endLine) {
        if (mdit_state_block_is_empty(state, nextLine)) {
            ++nextLine;
            continue;
        }
        if (mdit_state_block_is_code_block(state, nextLine)) {
            ++nextLine;
            last = nextLine;
            continue;
        }
        break;
    }
    state->line = last;

    mdit_token *t = mdit_state_block_push(state,
        MDIT_STR_LIT("code_block"), MDIT_STR_LIT("code"), 0);
    if (t == NULL) return false;

    mdit_buf raw;
    mdit_buf_init(&raw);
    (void)mdit_state_block_get_lines(state, startLine, last,
                                     4 + state->blkIndent, false, &raw);
    /* Append trailing '\n' to match upstream. */
    (void)mdit_buf_append_byte(&raw, '\n');
    t->content = arena_copy_str(state->arena, raw.data, raw.len);
    mdit_buf_destroy(&raw);
    mdit_token_set_map(t, startLine, state->line);
    return true;
}

/* ---------------------------------------------------------------------
 * Built-in rule: fence (``` / ~~~).
 * ------------------------------------------------------------------- */
static bool block_fence(mdit_state_block *state)
{
    int32_t startLine = state->cur_start_line;
    int32_t endLine   = state->cur_end_line;
    bool    silent    = state->cur_silent;

    int32_t pos     = state->bMarks[startLine] + state->tShift[startLine];
    int32_t maximum = state->eMarks[startLine];

    if (mdit_state_block_is_code_block(state, startLine)) return false;
    if (pos + 3 > maximum) return false;

    char marker = state->src.data[pos];
    if (marker != '`' && marker != '~') return false;

    int32_t mem = pos;
    pos = mdit_state_block_skip_chars(state, pos, marker);
    int32_t length = pos - mem;
    if (length < 3) return false;

    mdit_str markup = (mdit_str){ state->src.data + mem, (size_t)(pos - mem) };
    mdit_str params = (mdit_str){ state->src.data + pos, (size_t)(maximum - pos) };

    /* Backtick-fenced blocks may not contain backticks in the info string. */
    if (marker == '`') {
        for (size_t i = 0; i < params.len; ++i) {
            if (params.data[i] == '`') return false;
        }
    }
    if (silent) return true;

    /* Find the closing fence. */
    int32_t nextLine = startLine;
    bool haveEndMarker = false;
    while (true) {
        ++nextLine;
        if (nextLine >= endLine) break;
        int32_t lp = state->bMarks[nextLine] + state->tShift[nextLine];
        int32_t lm = state->eMarks[nextLine];
        int32_t lmem = lp;
        if (lp < lm && state->sCount[nextLine] < state->blkIndent) break;
        if (lp >= (int32_t)state->src.len) break;
        if (state->src.data[lp] != marker) continue;
        if (mdit_state_block_is_code_block(state, nextLine)) continue;
        lp = mdit_state_block_skip_chars(state, lp, marker);
        if (lp - lmem < length) continue;
        lp = mdit_state_block_skip_spaces(state, lp);
        if (lp < lm) continue;
        haveEndMarker = true;
        break;
    }

    int32_t outer_indent = state->sCount[startLine];
    state->line = nextLine + (haveEndMarker ? 1 : 0);

    mdit_token *t = mdit_state_block_push(state,
        MDIT_STR_LIT("fence"), MDIT_STR_LIT("code"), 0);
    if (t == NULL) return false;
    /* Upstream keeps the raw info string (including leading/trailing
     * whitespace) on the token. The renderer is responsible for
     * unescaping and splitting/trimming it when deriving a class name. */
    if (params.len > 0) {
        t->info = arena_copy_str(state->arena, params.data, params.len);
    }
    mdit_buf raw;
    mdit_buf_init(&raw);
    (void)mdit_state_block_get_lines(state, startLine + 1, nextLine,
                                     outer_indent, true, &raw);
    t->content = arena_copy_str(state->arena, raw.data, raw.len);
    mdit_buf_destroy(&raw);
    t->markup = arena_copy_str(state->arena, markup.data, markup.len);
    mdit_token_set_map(t, startLine, state->line);
    return true;
}

/* ---------------------------------------------------------------------
 * Built-in rule: hr (--- / *** / ___).
 * ------------------------------------------------------------------- */
static bool block_hr(mdit_state_block *state)
{
    int32_t startLine = state->cur_start_line;
    bool    silent    = state->cur_silent;
    int32_t pos     = state->bMarks[startLine] + state->tShift[startLine];
    int32_t maximum = state->eMarks[startLine];

    if (mdit_state_block_is_code_block(state, startLine)) return false;
    if (pos >= maximum) return false;
    char marker = state->src.data[pos++];
    if (marker != '*' && marker != '-' && marker != '_') return false;

    int32_t cnt = 1;
    while (pos < maximum) {
        char ch = state->src.data[pos++];
        if (ch != marker && !char_is_space((unsigned char)ch)) return false;
        if (ch == marker) ++cnt;
    }
    if (cnt < 3) return false;
    if (silent) return true;

    state->line = startLine + 1;
    mdit_token *t = mdit_state_block_push(state,
        MDIT_STR_LIT("hr"), MDIT_STR_LIT("hr"), 0);
    if (t == NULL) return false;
    mdit_token_set_map(t, startLine, state->line);

    /* markup = marker repeated (cnt + 1) times — matches upstream. */
    int32_t mlen = cnt + 1;
    char *m = (char *)mdit_arena_alloc(state->arena, (size_t)mlen);
    memset(m, marker, (size_t)mlen);
    t->markup = (mdit_str){ m, (size_t)mlen };
    return true;
}

/* ---------------------------------------------------------------------
 * Built-in rule: heading (ATX: # / ## ... up to ######).
 * ------------------------------------------------------------------- */
static bool block_heading(mdit_state_block *state)
{
    int32_t startLine = state->cur_start_line;
    bool    silent    = state->cur_silent;
    int32_t pos     = state->bMarks[startLine] + state->tShift[startLine];
    int32_t maximum = state->eMarks[startLine];

    if (mdit_state_block_is_code_block(state, startLine)) return false;
    if (pos >= maximum) return false;
    if (state->src.data[pos] != '#') return false;

    int32_t level = 1;
    ++pos;
    while (pos < maximum && state->src.data[pos] == '#' && level <= 6) {
        ++level;
        ++pos;
    }
    if (level > 6) return false;
    if (pos < maximum &&
        !char_is_space((unsigned char)state->src.data[pos])) return false;
    if (silent) return true;

    /* Cut tails like "    ###  " from the end of string. */
    int32_t end = mdit_state_block_skip_spaces_back(state, maximum, pos);
    int32_t tmp = mdit_state_block_skip_chars_back(state, end, '#', pos);
    if (tmp > pos &&
        char_is_space((unsigned char)state->src.data[tmp - 1])) end = tmp;

    state->line = startLine + 1;

    /* tag = "h<level>", markup = "######"[:level]. */
    char tag_buf[3]; tag_buf[0] = 'h'; tag_buf[1] = (char)('0' + level); tag_buf[2] = 0;
    mdit_str tag = arena_copy_str(state->arena, tag_buf, 2);
    char hashes[6] = { '#','#','#','#','#','#' };
    mdit_str markup = arena_copy_str(state->arena, hashes, (size_t)level);

    mdit_token *open = mdit_state_block_push(state,
        MDIT_STR_LIT("heading_open"), tag, 1);
    if (open == NULL) return false;
    open->markup = markup;
    mdit_token_set_map(open, startLine, state->line);

    mdit_str content_view = (mdit_str){ state->src.data + pos, (size_t)(end - pos) };
    mdit_str content = str_strip(content_view);

    mdit_token *inl = mdit_state_block_push(state,
        MDIT_STR_LIT("inline"), MDIT_STR_LIT(""), 0);
    if (inl == NULL) return false;
    if (content.len > 0) {
        inl->content = arena_copy_str(state->arena, content.data, content.len);
    }
    mdit_token_set_map(inl, startLine, state->line);
    mdit_token_set_children_empty(inl);

    mdit_token *close = mdit_state_block_push(state,
        MDIT_STR_LIT("heading_close"), tag, -1);
    if (close == NULL) return false;
    close->markup = markup;
    return true;
}

/* ---------------------------------------------------------------------
 * Built-in rule: lheading (Setext: title\n=== or title\n---).
 * ------------------------------------------------------------------- */
static bool block_lheading(mdit_state_block *state)
{
    int32_t startLine = state->cur_start_line;
    int32_t endLine   = state->cur_end_line;

    if (mdit_state_block_is_code_block(state, startLine)) return false;

    mdit_ruler *ruler = state->md->block.ruler;
    size_t n_term = 0;
    const mdit_rule_entry *terminators =
        mdit_ruler_get_rules(ruler, MDIT_STR_LIT("paragraph"), &n_term);

    mdit_str old_parent = state->parent_type;
    state->parent_type = MDIT_STR_LIT("paragraph");

    int32_t level = 0;
    char    marker = 0;
    int32_t nextLine = startLine + 1;
    while (nextLine < endLine && !mdit_state_block_is_empty(state, nextLine)) {
        if (state->sCount[nextLine] - state->blkIndent > 3) {
            ++nextLine;
            continue;
        }
        if (state->sCount[nextLine] >= state->blkIndent) {
            int32_t pos = state->bMarks[nextLine] + state->tShift[nextLine];
            int32_t mx  = state->eMarks[nextLine];
            if (pos < mx) {
                char m = state->src.data[pos];
                if (m == '-' || m == '=') {
                    pos = mdit_state_block_skip_chars(state, pos, m);
                    pos = mdit_state_block_skip_spaces(state, pos);
                    if (pos >= mx) {
                        marker = m;
                        level  = (m == '=') ? 1 : 2;
                        break;
                    }
                }
            }
        }
        if (state->sCount[nextLine] < 0) {
            ++nextLine;
            continue;
        }
        bool terminate = false;
        for (size_t i = 0; i < n_term; ++i) {
            mdit_block_rule_fn fn = (mdit_block_rule_fn)terminators[i].fn;
            int32_t ssl = state->cur_start_line, sel = state->cur_end_line;
            bool    sst = state->cur_silent;
            state->cur_start_line = nextLine;
            state->cur_end_line   = endLine;
            state->cur_silent     = true;
            bool matched = fn(state);
            state->cur_start_line = ssl; state->cur_end_line = sel;
            state->cur_silent     = sst;
            if (matched) { terminate = true; break; }
        }
        if (terminate) break;
        ++nextLine;
    }

    if (level == 0) {
        state->parent_type = old_parent;
        return false;
    }

    mdit_buf raw;
    mdit_buf_init(&raw);
    (void)mdit_state_block_get_lines(state, startLine, nextLine,
                                     state->blkIndent, false, &raw);
    mdit_str body = str_strip((mdit_str){ raw.data ? raw.data : "", raw.len });
    mdit_str content = (body.len > 0)
        ? arena_copy_str(state->arena, body.data, body.len)
        : MDIT_STR_LIT("");
    mdit_buf_destroy(&raw);

    state->line = nextLine + 1;

    char tag_buf[3]; tag_buf[0] = 'h'; tag_buf[1] = (char)('0' + level); tag_buf[2] = 0;
    mdit_str tag = arena_copy_str(state->arena, tag_buf, 2);
    mdit_str markup = arena_copy_str(state->arena, &marker, 1);

    mdit_token *open = mdit_state_block_push(state,
        MDIT_STR_LIT("heading_open"), tag, 1);
    if (open == NULL) { state->parent_type = old_parent; return false; }
    open->markup = markup;
    mdit_token_set_map(open, startLine, state->line);

    mdit_token *inl = mdit_state_block_push(state,
        MDIT_STR_LIT("inline"), MDIT_STR_LIT(""), 0);
    if (inl == NULL) { state->parent_type = old_parent; return false; }
    inl->content = content;
    mdit_token_set_map(inl, startLine, state->line - 1);
    mdit_token_set_children_empty(inl);

    mdit_token *close = mdit_state_block_push(state,
        MDIT_STR_LIT("heading_close"), tag, -1);
    if (close == NULL) { state->parent_type = old_parent; return false; }
    close->markup = markup;

    state->parent_type = old_parent;
    return true;
}

/* ---------------------------------------------------------------------
 * Built-in rule: list (bullet `*`/`-`/`+` and ordered `\d+[.)]`).
 *
 * Fairly mechanical port of rules_block/list.py. Skips the GFM
 * tasklist extension (md.options.tasklists) — that lands later when
 * we wire the option through. The post-pass `markTightParagraphs`
 * mirrors upstream: walk the just-emitted token slice and mark
 * paragraph open/close tokens hidden when the list is "tight".
 * ------------------------------------------------------------------- */
static int32_t skip_bullet_marker(mdit_state_block *state, int32_t startLine)
{
    int32_t pos     = state->bMarks[startLine] + state->tShift[startLine];
    int32_t maximum = state->eMarks[startLine];
    if (pos >= (int32_t)state->src.len) return -1;
    char marker = state->src.data[pos];
    ++pos;
    if (marker != '*' && marker != '-' && marker != '+') return -1;
    if (pos < maximum) {
        char ch = state->src.data[pos];
        if (!char_is_space((unsigned char)ch)) return -1;
    }
    return pos;
}

static int32_t skip_ordered_marker(mdit_state_block *state, int32_t startLine)
{
    int32_t start   = state->bMarks[startLine] + state->tShift[startLine];
    int32_t pos     = start;
    int32_t maximum = state->eMarks[startLine];
    if (pos + 1 >= maximum) return -1;
    if (pos >= (int32_t)state->src.len) return -1;
    unsigned char ch = (unsigned char)state->src.data[pos];
    ++pos;
    if (ch < '0' || ch > '9') return -1;
    for (;;) {
        if (pos >= maximum) return -1;
        ch = (unsigned char)state->src.data[pos];
        ++pos;
        if (ch >= '0' && ch <= '9') {
            if (pos - start >= 10) return -1;
            continue;
        }
        if (ch == ')' || ch == '.') break;
        return -1;
    }
    if (pos < maximum) {
        ch = (unsigned char)state->src.data[pos];
        if (!char_is_space(ch)) return -1;
    }
    return pos;
}

static void mark_tight_paragraphs(mdit_state_block *state, size_t list_idx)
{
    int32_t target = state->level + 2;
    size_t i = list_idx + 2;
    /* Python uses `len(state.tokens) - 2` so the trailing list_close
     * isn't touched. */
    if (state->tokens->len < 2) return;
    size_t end = state->tokens->len - 2;
    while (i < end) {
        mdit_token *t = &state->tokens->data[i];
        if (t->level == target && mdit_str_eq_z(t->type, "paragraph_open")) {
            state->tokens->data[i + 2].hidden = true; /* paragraph_close */
            t->hidden = true;
            i += 2;
        }
        ++i;
    }
}

static bool block_list(mdit_state_block *state)
{
    int32_t startLine = state->cur_start_line;
    int32_t endLine   = state->cur_end_line;
    bool    silent    = state->cur_silent;

    bool    tight = true;
    bool    is_terminating_paragraph = false;

    if (mdit_state_block_is_code_block(state, startLine)) return false;

    /* Lazy-continuation paragraph nested 4+ deeper than current list
     * marker -> not a fresh list. */
    if (state->listIndent >= 0 &&
        state->sCount[startLine] - state->listIndent >= 4 &&
        state->sCount[startLine] < state->blkIndent) {
        return false;
    }

    if (silent &&
        mdit_str_eq_z(state->parent_type, "paragraph") &&
        state->sCount[startLine] >= state->blkIndent) {
        is_terminating_paragraph = true;
    }

    int32_t pos_after_marker;
    int32_t marker_start = state->bMarks[startLine] + state->tShift[startLine];
    bool    is_ordered;
    int64_t marker_value = 1;

    pos_after_marker = skip_ordered_marker(state, startLine);
    if (pos_after_marker >= 0) {
        is_ordered = true;
        /* Parse the digit run as an integer. */
        marker_value = 0;
        for (int32_t i = marker_start; i < pos_after_marker - 1; ++i) {
            marker_value = marker_value * 10 +
                           (state->src.data[i] - '0');
        }
        if (is_terminating_paragraph && marker_value != 1) return false;
    } else {
        pos_after_marker = skip_bullet_marker(state, startLine);
        if (pos_after_marker < 0) return false;
        is_ordered = false;
    }

    if (is_terminating_paragraph &&
        mdit_state_block_skip_spaces(state, pos_after_marker) >=
            state->eMarks[startLine]) {
        return false;
    }

    char marker_char = state->src.data[pos_after_marker - 1];
    if (silent) return true;

    size_t list_tok_idx = state->tokens->len;

    mdit_token *open;
    if (is_ordered) {
        open = mdit_state_block_push(state,
            MDIT_STR_LIT("ordered_list_open"), MDIT_STR_LIT("ol"), 1);
        if (open == NULL) return false;
        if (marker_value != 1) {
            (void)mdit_token_attr_set_z(open, "start",
                                        mdit_value_int(marker_value));
        }
    } else {
        open = mdit_state_block_push(state,
            MDIT_STR_LIT("bullet_list_open"), MDIT_STR_LIT("ul"), 1);
        if (open == NULL) return false;
    }
    mdit_token_set_map(open, startLine, 0);
    open->markup = arena_copy_str(state->arena, &marker_char, 1);

    int32_t  nextLine          = startLine;
    bool     prev_empty_end    = false;
    size_t   n_term            = 0;
    const mdit_rule_entry *terminators =
        mdit_ruler_get_rules(state->md->block.ruler,
                             MDIT_STR_LIT("list"), &n_term);

    mdit_str old_parent = state->parent_type;
    state->parent_type = MDIT_STR_LIT("list");

    while (nextLine < endLine) {
        int32_t pos     = pos_after_marker;
        int32_t maximum = state->eMarks[nextLine];
        int32_t initial = state->sCount[nextLine] + pos_after_marker -
                          (state->bMarks[startLine] + state->tShift[startLine]);
        int32_t offset  = initial;

        while (pos < maximum) {
            char ch = state->src.data[pos];
            if (ch == '\t') {
                offset += 4 - (offset + state->bsCount[nextLine]) % 4;
            } else if (ch == ' ') {
                ++offset;
            } else {
                break;
            }
            ++pos;
        }
        int32_t content_start = pos;
        int32_t indent_after_marker;
        if (content_start >= maximum) {
            indent_after_marker = 1;
        } else {
            indent_after_marker = offset - initial;
        }
        if (indent_after_marker > 4) indent_after_marker = 1;
        int32_t indent = initial + indent_after_marker;

        mdit_token *li = mdit_state_block_push(state,
            MDIT_STR_LIT("list_item_open"), MDIT_STR_LIT("li"), 1);
        if (li == NULL) goto done;
        li->markup = arena_copy_str(state->arena, &marker_char, 1);
        size_t li_token_idx = state->tokens->len - 1;
        mdit_token_set_map(li, startLine, 0);
        if (is_ordered) {
            li->info = arena_copy_str(state->arena,
                state->src.data + marker_start,
                (size_t)(pos_after_marker - 1 - marker_start));
        }

        /* GFM task checkbox detection — mirrors upstream's `tasklists`
         * option. We stamp `meta["checked"]` on the list_item_open
         * token and advance bMarks past the checkbox so the inner
         * tokenize doesn't see it. The post-list pass below adds the
         * `task-list-item` / `contains-task-list` classes. */
        int     checkbox_len = 0;
        if (state->md->options.tasklists && content_start < maximum) {
            int chk = detect_task_checkbox(state->src.data,
                                           content_start, maximum);
            if (chk >= 0) {
                /* Use meta map (not attrs) — upstream stores `checked`
                 * here so the renderer can branch on it. */
                (void)mdit_map_set_z(&li->meta, "checked",
                                     mdit_value_bool(chk == 1));
                checkbox_len = 4;
            }
        }

        bool    old_tight    = state->tight;
        int32_t old_b_mark   = state->bMarks[startLine];
        int32_t old_t_shift  = state->tShift[startLine];
        int32_t old_s_count  = state->sCount[startLine];
        int32_t old_list_indent = state->listIndent;

        state->listIndent = state->blkIndent;
        state->blkIndent  = indent;
        state->tight      = true;
        state->tShift[startLine] = content_start - state->bMarks[startLine];
        state->sCount[startLine] = offset;

        if (checkbox_len) {
            state->bMarks[startLine] = content_start + checkbox_len;
            state->tShift[startLine] = 0;
        }

        if (content_start >= maximum &&
            mdit_state_block_is_empty(state, startLine + 1)) {
            int32_t bumped = state->line + 2;
            state->line = (bumped < endLine) ? bumped : endLine;
        } else {
            mdit_parser_block_tokenize(&state->md->block, state,
                                       startLine, endLine);
        }

        if (!state->tight || prev_empty_end) tight = false;

        prev_empty_end = (state->line - startLine) > 1 &&
                         mdit_state_block_is_empty(state, state->line - 1);

        state->blkIndent = state->listIndent;
        state->listIndent = old_list_indent;
        state->bMarks[startLine] = old_b_mark;
        state->tShift[startLine] = old_t_shift;
        state->sCount[startLine] = old_s_count;
        state->tight = old_tight;

        mdit_token *close = mdit_state_block_push(state,
            MDIT_STR_LIT("list_item_close"), MDIT_STR_LIT("li"), -1);
        if (close == NULL) goto done;
        close->markup = arena_copy_str(state->arena, &marker_char, 1);

        nextLine = startLine = state->line;
        /* Patch the list_item_open's map[1]. */
        state->tokens->data[li_token_idx].map.end = nextLine;

        if (nextLine >= endLine) break;
        if (state->sCount[nextLine] < state->blkIndent) break;
        if (mdit_state_block_is_code_block(state, startLine)) break;

        bool terminate = false;
        for (size_t i = 0; i < n_term; ++i) {
            mdit_block_rule_fn fn = (mdit_block_rule_fn)terminators[i].fn;
            int32_t ssl = state->cur_start_line, sel = state->cur_end_line;
            bool    sst = state->cur_silent;
            state->cur_start_line = nextLine;
            state->cur_end_line   = endLine;
            state->cur_silent     = true;
            bool matched = fn(state);
            state->cur_start_line = ssl; state->cur_end_line = sel;
            state->cur_silent     = sst;
            if (matched) { terminate = true; break; }
        }
        if (terminate) break;

        if (is_ordered) {
            pos_after_marker = skip_ordered_marker(state, nextLine);
            if (pos_after_marker < 0) break;
            marker_start = state->bMarks[nextLine] + state->tShift[nextLine];
        } else {
            pos_after_marker = skip_bullet_marker(state, nextLine);
            if (pos_after_marker < 0) break;
        }
        if (marker_char != state->src.data[pos_after_marker - 1]) break;
    }

    /* Tasklists: walk the immediate-child `list_item_open` tokens, and
     * if any carry `meta["checked"]`, stamp `task-list-item` on them
     * and `contains-task-list` on the outer `*_list_open` token. The
     * level check matches upstream — only items directly under this
     * list, not nested-list items, get the classes. */
    if (state->md->options.tasklists) {
        bool contains_task = false;
        int32_t list_level = state->tokens->data[list_tok_idx].level;
        for (size_t j = list_tok_idx + 1; j < state->tokens->len; ++j) {
            mdit_token *tk = &state->tokens->data[j];
            if (tk->level != list_level + 1) continue;
            if (!mdit_str_eq_z(tk->type, "list_item_open")) continue;
            if (mdit_map_get_z(&tk->meta, "checked") == NULL) continue;
            (void)mdit_token_attr_join(tk, MDIT_STR_LIT("class"),
                                       MDIT_STR_LIT("task-list-item"));
            contains_task = true;
        }
        if (contains_task) {
            (void)mdit_token_attr_join(&state->tokens->data[list_tok_idx],
                                       MDIT_STR_LIT("class"),
                                       MDIT_STR_LIT("contains-task-list"));
        }
    }

    mdit_token *close = mdit_state_block_push(state,
        is_ordered ? MDIT_STR_LIT("ordered_list_close")
                   : MDIT_STR_LIT("bullet_list_close"),
        is_ordered ? MDIT_STR_LIT("ol") : MDIT_STR_LIT("ul"),
        -1);
    if (close == NULL) goto done;
    close->markup = arena_copy_str(state->arena, &marker_char, 1);

    /* Patch list_open's map[1]. */
    state->tokens->data[list_tok_idx].map.end = nextLine;
    state->line = nextLine;

    if (tight) mark_tight_paragraphs(state, list_tok_idx);

done:
    state->parent_type = old_parent;
    return true;
}

/* ---------------------------------------------------------------------
 * Built-in rule: blockquote (> ...).
 *
 * Direct port of rules_block.blockquote. Maintains parallel "old"
 * arrays so it can restore bMarks / sCount / tShift / bsCount after
 * recursing into nested blocks. The GFM-alert extension is omitted
 * for now (it lands when we wire up `md.options.alerts`).
 * ------------------------------------------------------------------- */
static bool block_blockquote(mdit_state_block *state)
{
    int32_t startLine = state->cur_start_line;
    int32_t endLine   = state->cur_end_line;
    bool    silent    = state->cur_silent;

    int32_t oldLineMax = state->lineMax;
    int32_t pos = state->bMarks[startLine] + state->tShift[startLine];
    int32_t mx  = state->eMarks[startLine];

    if (mdit_state_block_is_code_block(state, startLine)) return false;
    if (pos >= (int32_t)state->src.len) return false;
    if (state->src.data[pos] != '>') return false;
    ++pos;
    if (silent) return true;

    int32_t initial = state->sCount[startLine] + 1;
    int32_t offset  = initial;
    bool    spaceAfterMarker;
    bool    adjustTab = false;

    char second_char = (pos < (int32_t)state->src.len)
                       ? state->src.data[pos] : 0;
    if (second_char == ' ') {
        ++pos; ++initial; ++offset;
        spaceAfterMarker = true;
    } else if (second_char == '\t') {
        spaceAfterMarker = true;
        if ((state->bsCount[startLine] + offset) % 4 == 3) {
            ++pos; ++initial; ++offset;
        } else {
            adjustTab = true;
        }
    } else {
        spaceAfterMarker = false;
    }

    /* Per-line "old" snapshots so we can restore on exit. We only ever
     * touch lines [startLine, nextLine), so a vec keyed by line offset
     * from startLine is enough. */
    mdit_vec_int32 oldB, oldBS, oldS, oldT;
    mdit_vec_int32_init(&oldB,  state->arena);
    mdit_vec_int32_init(&oldBS, state->arena);
    mdit_vec_int32_init(&oldS,  state->arena);
    mdit_vec_int32_init(&oldT,  state->arena);

    (void)mdit_vec_int32_push(&oldB, state->bMarks[startLine]);
    state->bMarks[startLine] = pos;

    while (pos < mx) {
        char ch = state->src.data[pos];
        if (char_is_space((unsigned char)ch)) {
            if (ch == '\t') {
                offset += 4 - (offset + state->bsCount[startLine] +
                               (adjustTab ? 1 : 0)) % 4;
            } else {
                ++offset;
            }
        } else {
            break;
        }
        ++pos;
    }
    bool lastLineEmpty = pos >= mx;

    (void)mdit_vec_int32_push(&oldBS, state->bsCount[startLine]);
    state->bsCount[startLine] = state->sCount[startLine] + 1 +
                                (spaceAfterMarker ? 1 : 0);
    (void)mdit_vec_int32_push(&oldS,  state->sCount[startLine]);
    state->sCount[startLine] = offset - initial;
    (void)mdit_vec_int32_push(&oldT,  state->tShift[startLine]);
    state->tShift[startLine] = pos - state->bMarks[startLine];

    size_t n_term = 0;
    const mdit_rule_entry *terminators =
        mdit_ruler_get_rules(state->md->block.ruler,
                             MDIT_STR_LIT("blockquote"), &n_term);

    mdit_str old_parent = state->parent_type;
    state->parent_type = MDIT_STR_LIT("blockquote");

    int32_t nextLine = startLine + 1;
    while (nextLine < endLine) {
        bool isOutdented = state->sCount[nextLine] < state->blkIndent;
        pos = state->bMarks[nextLine] + state->tShift[nextLine];
        mx  = state->eMarks[nextLine];
        if (pos >= mx) break; /* Case 1: empty line outside */

        bool inside = (state->src.data[pos] == '>') && !isOutdented;
        ++pos;
        if (inside) {
            initial = state->sCount[nextLine] + 1;
            offset  = initial;
            adjustTab = false;
            char nc = (pos < (int32_t)state->src.len)
                      ? state->src.data[pos] : 0;
            if (nc == ' ') {
                ++pos; ++initial; ++offset; spaceAfterMarker = true;
            } else if (nc == '\t') {
                spaceAfterMarker = true;
                if ((state->bsCount[nextLine] + offset) % 4 == 3) {
                    ++pos; ++initial; ++offset;
                } else {
                    adjustTab = true;
                }
            } else {
                spaceAfterMarker = false;
            }
            (void)mdit_vec_int32_push(&oldB, state->bMarks[nextLine]);
            state->bMarks[nextLine] = pos;
            while (pos < mx) {
                char ch = state->src.data[pos];
                if (char_is_space((unsigned char)ch)) {
                    if (ch == '\t') {
                        offset += 4 - (offset + state->bsCount[nextLine] +
                                       (adjustTab ? 1 : 0)) % 4;
                    } else {
                        ++offset;
                    }
                } else {
                    break;
                }
                ++pos;
            }
            lastLineEmpty = pos >= mx;
            (void)mdit_vec_int32_push(&oldBS, state->bsCount[nextLine]);
            state->bsCount[nextLine] = state->sCount[nextLine] + 1 +
                                       (spaceAfterMarker ? 1 : 0);
            (void)mdit_vec_int32_push(&oldS, state->sCount[nextLine]);
            state->sCount[nextLine] = offset - initial;
            (void)mdit_vec_int32_push(&oldT, state->tShift[nextLine]);
            state->tShift[nextLine] = pos - state->bMarks[nextLine];
            ++nextLine;
            continue;
        }

        if (lastLineEmpty) break; /* Case 2 */

        /* Case 3: another tag */
        bool terminate = false;
        for (size_t i = 0; i < n_term; ++i) {
            mdit_block_rule_fn fn = (mdit_block_rule_fn)terminators[i].fn;
            int32_t ssl = state->cur_start_line, sel = state->cur_end_line;
            bool    sst = state->cur_silent;
            state->cur_start_line = nextLine;
            state->cur_end_line   = endLine;
            state->cur_silent     = true;
            bool matched = fn(state);
            state->cur_start_line = ssl; state->cur_end_line = sel;
            state->cur_silent     = sst;
            if (matched) { terminate = true; break; }
        }
        if (terminate) {
            state->lineMax = nextLine;
            if (state->blkIndent != 0) {
                (void)mdit_vec_int32_push(&oldB,  state->bMarks[nextLine]);
                (void)mdit_vec_int32_push(&oldBS, state->bsCount[nextLine]);
                (void)mdit_vec_int32_push(&oldT,  state->tShift[nextLine]);
                (void)mdit_vec_int32_push(&oldS,  state->sCount[nextLine]);
                state->sCount[nextLine] -= state->blkIndent;
            }
            break;
        }
        (void)mdit_vec_int32_push(&oldB,  state->bMarks[nextLine]);
        (void)mdit_vec_int32_push(&oldBS, state->bsCount[nextLine]);
        (void)mdit_vec_int32_push(&oldT,  state->tShift[nextLine]);
        (void)mdit_vec_int32_push(&oldS,  state->sCount[nextLine]);
        state->sCount[nextLine] = -1;
        ++nextLine;
    }

    int32_t oldIndent = state->blkIndent;
    state->blkIndent = 0;

    /* GitHub-style alert detection: when `options.alerts` is on and
     * the blockquote has at least one content line beyond the marker
     * line, peek for `[!NOTE]` / `[!TIP]` / etc. on the first content
     * line. If matched, emit alert tokens (`alert_open` / title / body
     * / `alert_close`) instead of `blockquote_open`/`_close`. Skip the
     * marker line during inner tokenisation so the alert kind doesn't
     * appear as paragraph text. */
    int alert_kind = -1;
    if (state->md->options.alerts && nextLine > startLine) {
        int32_t a_pos = state->bMarks[startLine] + state->tShift[startLine];
        int32_t a_max = state->eMarks[startLine];
        if (a_pos < a_max) {
            alert_kind = detect_alert(state->src.data, a_pos, a_max);
        }
    }

    size_t open_idx = state->tokens->len;
    if (alert_kind >= 0) {
        const char *kind_name = g_alert_kinds[alert_kind].name;
        size_t      kind_len  = g_alert_kinds[alert_kind].len;
        const char *cap_name  = g_alert_kinds[alert_kind].capitalised;

        /* Compose `markdown-alert markdown-alert-<lower>`. The lower
         * form is just the original NOTE/TIP/.. ascii-lowercased; we
         * synthesise it inline from `kind_name`. */
        size_t prefix_len = sizeof "markdown-alert markdown-alert-" - 1;
        char  *cls = (char *)mdit_arena_alloc(state->arena,
                                              prefix_len + kind_len);
        if (cls == NULL) goto restore;
        memcpy(cls, "markdown-alert markdown-alert-", prefix_len);
        for (size_t i = 0; i < kind_len; ++i) {
            char c = kind_name[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            cls[prefix_len + i] = c;
        }
        mdit_str cls_str = { cls, prefix_len + kind_len };
        mdit_str kind_str_upper = arena_copy_str(state->arena,
                                                 kind_name, kind_len);

        mdit_token *open = mdit_state_block_push(state,
            MDIT_STR_LIT("alert_open"), MDIT_STR_LIT("div"), 1);
        if (open == NULL) goto restore;
        open->markup = MDIT_STR_LIT(">");
        (void)mdit_token_attr_set_z(open, "class",
                                    mdit_value_str(cls_str));
        mdit_token_set_map(open, startLine, 0);
        open->info = kind_str_upper;
        (void)mdit_map_set_z(&open->meta, "kind",
                             mdit_value_str(kind_str_upper));

        mdit_token *t_open = mdit_state_block_push(state,
            MDIT_STR_LIT("alert_title_open"), MDIT_STR_LIT("p"), 1);
        if (t_open == NULL) goto restore;
        (void)mdit_token_attr_set_z(t_open, "class",
            mdit_value_str(MDIT_STR_LIT("markdown-alert-title")));

        mdit_token *t_inline = mdit_state_block_push(state,
            MDIT_STR_LIT("inline"), MDIT_STR_LIT(""), 0);
        if (t_inline == NULL) goto restore;
        t_inline->content = arena_copy_str(state->arena,
                                           cap_name, strlen(cap_name));
        mdit_token_set_children_empty(t_inline);

        mdit_token *t_close = mdit_state_block_push(state,
            MDIT_STR_LIT("alert_title_close"), MDIT_STR_LIT("p"), -1);
        if (t_close == NULL) goto restore;

        int32_t content_start = startLine + 1;
        if (content_start < nextLine) {
            mdit_parser_block_tokenize(&state->md->block, state,
                                       content_start, nextLine);
        } else {
            state->line = nextLine;
        }

        mdit_token *close = mdit_state_block_push(state,
            MDIT_STR_LIT("alert_close"), MDIT_STR_LIT("div"), -1);
        if (close == NULL) goto restore;
        close->markup = MDIT_STR_LIT(">");
    } else {
        mdit_token *open = mdit_state_block_push(state,
            MDIT_STR_LIT("blockquote_open"), MDIT_STR_LIT("blockquote"), 1);
        if (open == NULL) goto restore;
        open->markup = MDIT_STR_LIT(">");
        mdit_token_set_map(open, startLine, 0);

        mdit_parser_block_tokenize(&state->md->block, state,
                                   startLine, nextLine);

        mdit_token *close = mdit_state_block_push(state,
            MDIT_STR_LIT("blockquote_close"), MDIT_STR_LIT("blockquote"), -1);
        if (close == NULL) goto restore;
        close->markup = MDIT_STR_LIT(">");
    }

    /* Patch open token's map[1] to state.line. The token vector may
     * grow during nested tokenization, so use the saved index rather
     * than the possibly-stale `open` pointer. */
    state->tokens->data[open_idx].map.end = state->line;

restore:
    state->lineMax     = oldLineMax;
    state->parent_type = old_parent;
    /* Restore per-line snapshots in the same order they were captured
     * (the Python code does `for i, item in enumerate(oldTShift)`,
     * which assumes contiguous lines starting at startLine). */
    for (size_t i = 0; i < oldT.len; ++i) {
        int32_t line = startLine + (int32_t)i;
        state->bMarks[line]  = oldB.data[i];
        state->tShift[line]  = oldT.data[i];
        state->sCount[line]  = oldS.data[i];
        state->bsCount[line] = oldBS.data[i];
    }
    state->blkIndent = oldIndent;
    return true;
}

/* ---------------------------------------------------------------------
 * Built-in rule: reference ([label]: destination "title").
 * ------------------------------------------------------------------- */
static bool block_reference(mdit_state_block *state)
{
    int32_t startLine = state->cur_start_line;
    bool    silent    = state->cur_silent;

    int32_t pos = state->bMarks[startLine] + state->tShift[startLine];
    int32_t maximum_line = state->eMarks[startLine];
    int32_t nextLine = startLine + 1;

    if (mdit_state_block_is_code_block(state, startLine)) return false;
    if (pos >= maximum_line || state->src.data[pos] != '[') return false;

    mdit_buf string;
    mdit_buf_init(&string);
    int32_t first_end = maximum_line + 1;
    if (first_end > (int32_t)state->src.len) first_end = (int32_t)state->src.len;
    if (!mdit_buf_append(&string, state->src.data + pos,
                         (size_t)(first_end - pos))) {
        mdit_buf_destroy(&string);
        return false;
    }

    size_t maximum = string.len;
    size_t labelEnd = 0;
    bool haveLabelEnd = false;
    size_t p = 1;
    while (p < maximum) {
        unsigned char ch = (unsigned char)string.data[p];
        if (ch == '[') {
            mdit_buf_destroy(&string);
            return false;
        } else if (ch == ']') {
            labelEnd = p;
            haveLabelEnd = true;
            break;
        } else if (ch == '\n') {
            if (append_reference_next_line(state, nextLine, &string)) {
                maximum = string.len;
                ++nextLine;
            }
        } else if (ch == '\\') {
            ++p;
            if (p < maximum && string.data[p] == '\n' &&
                append_reference_next_line(state, nextLine, &string)) {
                maximum = string.len;
                ++nextLine;
            }
        }
        ++p;
    }

    if (!haveLabelEnd || labelEnd + 1 >= maximum ||
        string.data[labelEnd + 1] != ':') {
        mdit_buf_destroy(&string);
        return false;
    }

    p = labelEnd + 2;
    while (p < maximum) {
        unsigned char ch = (unsigned char)string.data[p];
        if (ch == '\n') {
            if (append_reference_next_line(state, nextLine, &string)) {
                maximum = string.len;
                ++nextLine;
            }
        } else if (!is_ref_space(ch)) {
            break;
        }
        ++p;
    }

    mdit_link_destination_result dest;
    if (!mdit_parse_link_destination(
            state->arena,
            (mdit_str){ string.data ? string.data : "", maximum },
            p, maximum, &dest) || !dest.ok) {
        mdit_buf_destroy(&string);
        return false;
    }

    mdit_str href;
    if (!mdit_normalize_link(state->arena, dest.str, &href) ||
        !mdit_validate_link(href)) {
        mdit_buf_destroy(&string);
        return false;
    }

    p = dest.pos;
    size_t destEndPos = p;
    int32_t destEndLineNo = nextLine;

    size_t start = p;
    while (p < maximum) {
        unsigned char ch = (unsigned char)string.data[p];
        if (ch == '\n') {
            if (append_reference_next_line(state, nextLine, &string)) {
                maximum = string.len;
                ++nextLine;
            }
        } else if (!is_ref_space(ch)) {
            break;
        }
        ++p;
    }

    mdit_link_title_result titleRes;
    if (!mdit_parse_link_title(
            state->arena,
            (mdit_str){ string.data ? string.data : "", maximum },
            p, maximum, NULL, &titleRes)) {
        mdit_buf_destroy(&string);
        return false;
    }
    while (titleRes.can_continue) {
        if (!append_reference_next_line(state, nextLine, &string)) break;
        p = maximum;
        maximum = string.len;
        ++nextLine;
        mdit_link_title_result cont;
        if (!mdit_parse_link_title(
                state->arena,
                (mdit_str){ string.data ? string.data : "", maximum },
                p, maximum, &titleRes, &cont)) {
            mdit_buf_destroy(&string);
            return false;
        }
        titleRes = cont;
    }

    mdit_str title = MDIT_STR_LIT("");
    if (p < maximum && start != p && titleRes.ok) {
        title = titleRes.str;
        p = titleRes.pos;
    } else {
        p = destEndPos;
        nextLine = destEndLineNo;
    }

    while (p < maximum) {
        unsigned char ch = (unsigned char)string.data[p];
        if (!is_ref_space(ch)) break;
        ++p;
    }

    if (p < maximum && string.data[p] != '\n' && title.len > 0) {
        title = MDIT_STR_LIT("");
        p = destEndPos;
        nextLine = destEndLineNo;
        while (p < maximum) {
            unsigned char ch = (unsigned char)string.data[p];
            if (!is_ref_space(ch)) break;
            ++p;
        }
    }

    if (p < maximum && string.data[p] != '\n') {
        mdit_buf_destroy(&string);
        return false;
    }

    mdit_str label = mdit_env_normalize_reference(
        state->arena,
        (mdit_str){ string.data + 1, labelEnd - 1 });
    if (label.len == 0) {
        mdit_buf_destroy(&string);
        return false;
    }

    if (silent) {
        mdit_buf_destroy(&string);
        return true;
    }

    state->line = nextLine;
    bool ok = mdit_env_add_reference((mdit_env *)state->env, label, href, title,
                                     startLine, state->line);
    mdit_buf_destroy(&string);
    return ok;
}

/* ---------------------------------------------------------------------
 * Built-in rule: html_block.
 *
 * Direct port of rules_block/html_block.py. Seven recognized sequences
 * each have an opener (anchored at line start) and a closer (anywhere
 * on a subsequent line, or "first empty line" for sequences 6/7). The
 * rule is gated on `options.html` — same as upstream.
 * ------------------------------------------------------------------- */

/* Case-insensitive ASCII prefix match: does s[0..n) start with `lit`? */
static bool ascii_iprefix(const char *s, size_t n, const char *lit)
{
    size_t i = 0;
    for (; lit[i] != '\0'; ++i) {
        if (i >= n) return false;
        unsigned char c = (unsigned char)s[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        if (c != (unsigned char)lit[i]) return false;
    }
    return true;
}

/* Find the first occurrence of `needle` (NUL-terminated, lowercase)
 * in haystack[0..n), case-insensitively. Returns true if found. */
static bool ascii_ifind(const char *hay, size_t n, const char *needle)
{
    size_t nl = 0;
    while (needle[nl] != '\0') ++nl;
    if (nl == 0 || nl > n) return false;
    size_t end = n - nl;
    for (size_t i = 0; i <= end; ++i) {
        if (ascii_iprefix(hay + i, n - i, needle)) return true;
    }
    return false;
}

/* Find the first occurrence of `needle[0..nl)` in `hay[0..n)`. */
static bool find_substr(const char *hay, size_t n,
                        const char *needle, size_t nl)
{
    if (nl == 0 || nl > n) return nl == 0;
    size_t end = n - nl;
    for (size_t i = 0; i <= end; ++i) {
        if (memcmp(hay + i, needle, nl) == 0) return true;
    }
    return false;
}

/* Match opening sequences against `text[0..len)`. Returns the sequence
 * index (1..7) on match, or 0 if none. */
typedef enum {
    HTML_SEQ_NONE = 0,
    HTML_SEQ_RAW = 1,    /* script / pre / style / textarea */
    HTML_SEQ_COMMENT,    /* <!-- */
    HTML_SEQ_PI,         /* <?  */
    HTML_SEQ_DECL,       /* <!X */
    HTML_SEQ_CDATA,      /* <![CDATA[ */
    HTML_SEQ_BLOCK_NAME, /* <[/]?blockname */
    HTML_SEQ_GENERIC,    /* arbitrary <tag>\s*$ */
} html_seq_t;

static html_seq_t html_match_open(const char *text, size_t len,
                                  bool *can_terminate_paragraph)
{
    if (len < 2 || text[0] != '<') return HTML_SEQ_NONE;
    *can_terminate_paragraph = true;

    /* Sequence 1: <(script|pre|style|textarea) followed by \s, >, or EOL. */
    {
        static const struct { const char *name; size_t len; } RAW[] = {
            { "script",   6 },
            { "pre",      3 },
            { "style",    5 },
            { "textarea", 8 },
        };
        for (size_t k = 0; k < sizeof RAW / sizeof RAW[0]; ++k) {
            size_t need = 1 + RAW[k].len;
            if (len < need) continue;
            if (!ascii_iprefix(text + 1, len - 1, RAW[k].name)) continue;
            if (need == len) return HTML_SEQ_RAW;
            unsigned char nx = (unsigned char)text[need];
            if (nx == ' ' || nx == '\t' || nx == '\n' || nx == '\r' ||
                nx == '\v' || nx == '\f' || nx == '>') {
                return HTML_SEQ_RAW;
            }
        }
    }

    /* Sequence 2: <!-- */
    if (len >= 4 && text[1] == '!' && text[2] == '-' && text[3] == '-') {
        return HTML_SEQ_COMMENT;
    }

    /* Sequence 3: <? */
    if (text[1] == '?') return HTML_SEQ_PI;

    /* Sequence 4: <![A-Z] */
    if (len >= 3 && text[1] == '!' &&
        (unsigned char)text[2] >= 'A' && (unsigned char)text[2] <= 'Z') {
        return HTML_SEQ_DECL;
    }

    /* Sequence 5: <![CDATA[ */
    if (len >= 9 && text[1] == '!' && text[2] == '[' &&
        memcmp(text + 3, "CDATA[", 6) == 0) {
        return HTML_SEQ_CDATA;
    }

    /* Sequence 6: <[/]?blockname followed by \s, /?>, or EOL. */
    {
        size_t p = 1;
        if (text[p] == '/' && p + 1 < len) ++p;
        size_t name_start = p;
        while (p < len) {
            unsigned char c = (unsigned char)text[p];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9')) ++p;
            else break;
        }
        size_t name_len = p - name_start;
        if (name_len > 0 &&
            mdit_html_is_block_name(text + name_start, name_len)) {
            bool ok = false;
            if (p == len) ok = true;
            else {
                unsigned char nx = (unsigned char)text[p];
                if (nx == ' ' || nx == '\t' || nx == '\n' || nx == '\r' ||
                    nx == '\v' || nx == '\f' || nx == '>') {
                    ok = true;
                } else if (nx == '/' && p + 1 < len && text[p + 1] == '>') {
                    ok = true;
                }
            }
            if (ok) return HTML_SEQ_BLOCK_NAME;
        }
    }

    /* Sequence 7: a complete open/close tag followed by \s* and EOL.
     *
     * Sequence 7 cannot terminate a paragraph -- mirroring upstream's
     * `False` flag on this entry. */
    {
        size_t consumed = mdit_html_match_open_close_tag(text, len);
        if (consumed > 0) {
            size_t q = consumed;
            while (q < len) {
                unsigned char nx = (unsigned char)text[q];
                if (nx == ' ' || (nx >= 0x09 && nx <= 0x0D)) ++q;
                else break;
            }
            if (q == len) {
                *can_terminate_paragraph = false;
                return HTML_SEQ_GENERIC;
            }
        }
    }
    return HTML_SEQ_NONE;
}

/* Test whether a closer for `seq` appears in the line text. */
static bool html_match_close(html_seq_t seq, const char *text, size_t len)
{
    switch (seq) {
        case HTML_SEQ_RAW:
            return ascii_ifind(text, len, "</script>") ||
                   ascii_ifind(text, len, "</pre>")    ||
                   ascii_ifind(text, len, "</style>")  ||
                   ascii_ifind(text, len, "</textarea>");
        case HTML_SEQ_COMMENT: return find_substr(text, len, "-->",  3);
        case HTML_SEQ_PI:      return find_substr(text, len, "?>",   2);
        case HTML_SEQ_DECL:    return find_substr(text, len, ">",    1);
        case HTML_SEQ_CDATA:   return find_substr(text, len, "]]>",  3);
        case HTML_SEQ_BLOCK_NAME:
        case HTML_SEQ_GENERIC: return len == 0;
        default:               return false;
    }
}

static bool block_html_block(mdit_state_block *state)
{
    int32_t startLine = state->cur_start_line;
    int32_t endLine   = state->cur_end_line;
    bool    silent    = state->cur_silent;

    int32_t pos = state->bMarks[startLine] + state->tShift[startLine];
    int32_t maximum = state->eMarks[startLine];

    if (mdit_state_block_is_code_block(state, startLine)) return false;
    if (!state->md->options.html) return false;
    if (pos >= maximum || state->src.data[pos] != '<') return false;

    const char *line0     = state->src.data + pos;
    size_t      line0_len = (size_t)(maximum - pos);

    bool can_terminate;
    html_seq_t seq = html_match_open(line0, line0_len, &can_terminate);
    if (seq == HTML_SEQ_NONE) return false;

    if (silent) return can_terminate;

    int32_t nextLine = startLine + 1;
    if (!html_match_close(seq, line0, line0_len)) {
        while (nextLine < endLine) {
            if (state->sCount[nextLine] < state->blkIndent) break;
            int32_t lp = state->bMarks[nextLine] + state->tShift[nextLine];
            int32_t lm = state->eMarks[nextLine];
            const char *line = state->src.data + lp;
            size_t      line_len = (size_t)(lm - lp);
            if (html_match_close(seq, line, line_len)) {
                if (line_len != 0) ++nextLine;
                break;
            }
            ++nextLine;
        }
    }

    state->line = nextLine;

    mdit_token *t = mdit_state_block_push(state,
        MDIT_STR_LIT("html_block"), MDIT_STR_LIT(""), 0);
    if (t == NULL) return false;
    mdit_token_set_map(t, startLine, nextLine);

    mdit_buf raw;
    mdit_buf_init(&raw);
    (void)mdit_state_block_get_lines(state, startLine, nextLine,
                                     state->blkIndent, true, &raw);
    t->content = arena_copy_str(state->arena, raw.data ? raw.data : "", raw.len);
    mdit_buf_destroy(&raw);
    return true;
}

/* ---------------------------------------------------------------------
 * Built-in rule: table (GFM-extension tables).
 *
 * Direct port of rules_block/table.py. Two-line header (caption row +
 * `:?-+:?` divider row) is required; subsequent lines fill the body
 * until termination, an empty line, or the autocomplete budget runs
 * out. Cell splitting honours `\|` escapes the same way the Python
 * implementation does.
 * ------------------------------------------------------------------- */
#define MDIT_TABLE_MAX_AUTOCOMPLETED 0x10000

/* Validate the trimmed divider cell against `^:?-+:?$`. */
static bool table_divider_cell_ok(mdit_str t)
{
    if (t.len == 0) return false;
    size_t i = 0;
    if (t.data[i] == ':') ++i;
    size_t dashes = 0;
    while (i < t.len && t.data[i] == '-') { ++i; ++dashes; }
    if (dashes == 0) return false;
    if (i < t.len && t.data[i] == ':') ++i;
    return i == t.len;
}

typedef enum {
    TABLE_ALIGN_NONE = 0,
    TABLE_ALIGN_LEFT,
    TABLE_ALIGN_CENTER,
    TABLE_ALIGN_RIGHT,
} table_align_t;

static const char *table_align_style(table_align_t a)
{
    switch (a) {
        case TABLE_ALIGN_LEFT:   return "text-align:left";
        case TABLE_ALIGN_CENTER: return "text-align:center";
        case TABLE_ALIGN_RIGHT:  return "text-align:right";
        default:                 return NULL;
    }
}

/* Typed dynamic vector of arena-borrowed string views, used to hold
 * table cells without leaning on mdit_buf for every split. */
typedef struct table_cells {
    mdit_str   *data;
    size_t      len;
    size_t      cap;
    mdit_arena *arena;
} table_cells;

static void table_cells_init(table_cells *c, mdit_arena *a)
{
    c->data = NULL; c->len = 0; c->cap = 0; c->arena = a;
}

static bool table_cells_push(table_cells *c, mdit_str s)
{
    if (c->len == c->cap) {
        size_t new_cap = (c->cap == 0) ? 8 : (c->cap * 2);
        mdit_str *next = (mdit_str *)mdit_arena_alloc(
            c->arena, new_cap * sizeof *next);
        if (c->len > 0 && c->data != NULL) {
            memcpy(next, c->data, c->len * sizeof *next);
        }
        c->data = next;
        c->cap = new_cap;
    }
    c->data[c->len++] = s;
    return true;
}

/* Direct port of rules_block.table.escapedSplit. Splits on unescaped
 * `|`. Backslash escapes for `|` collapse to a literal `|` in the
 * cell content. The output cells are arena-owned copies; the caller
 * doesn't have to worry about lifetime. */
static bool table_escaped_split(mdit_arena *arena, mdit_str line,
                                table_cells *out)
{
    /* Worst case: one cell per byte. We accumulate into a scratch buf
     * then snapshot into the arena per cell. */
    mdit_buf cur;
    mdit_buf_init(&cur);
    size_t pos = 0;
    size_t last_pos = 0;
    bool   is_escaped = false;
    bool   ok = true;
    while (pos < line.len) {
        unsigned char ch = (unsigned char)line.data[pos];
        if (ch == '|') {
            if (!is_escaped) {
                if (pos > last_pos) {
                    if (!mdit_buf_append(&cur, line.data + last_pos,
                                         pos - last_pos)) { ok = false; break; }
                }
                mdit_str cell = MDIT_STR_LIT("");
                if (cur.len > 0) {
                    char *buf = (char *)mdit_arena_alloc(arena, cur.len);
                    memcpy(buf, cur.data, cur.len);
                    cell.data = buf; cell.len = cur.len;
                }
                if (!table_cells_push(out, cell)) { ok = false; break; }
                mdit_buf_reset(&cur);
                last_pos = pos + 1;
            } else {
                /* Escaped pipe: keep [last_pos, pos - 1) then start at pos. */
                if (pos > last_pos + 1) {
                    if (!mdit_buf_append(&cur, line.data + last_pos,
                                         pos - 1 - last_pos)) {
                        ok = false; break;
                    }
                }
                last_pos = pos;
            }
        }
        is_escaped = (ch == '\\');
        ++pos;
    }
    if (ok) {
        if (line.len > last_pos) {
            if (!mdit_buf_append(&cur, line.data + last_pos,
                                 line.len - last_pos)) ok = false;
        }
        if (ok) {
            mdit_str cell = MDIT_STR_LIT("");
            if (cur.len > 0) {
                char *buf = (char *)mdit_arena_alloc(arena, cur.len);
                memcpy(buf, cur.data, cur.len);
                cell.data = buf; cell.len = cur.len;
            }
            ok = table_cells_push(out, cell);
        }
    }
    mdit_buf_destroy(&cur);
    return ok;
}

static mdit_str table_get_line(const mdit_state_block *s, int32_t line)
{
    int32_t pos = s->bMarks[line] + s->tShift[line];
    int32_t maximum = s->eMarks[line];
    return (mdit_str){ s->src.data + pos, (size_t)(maximum - pos) };
}

static bool table_contains_byte(mdit_str s, char c)
{
    for (size_t i = 0; i < s.len; ++i) {
        if (s.data[i] == c) return true;
    }
    return false;
}

static bool block_table(mdit_state_block *state)
{
    int32_t startLine = state->cur_start_line;
    int32_t endLine   = state->cur_end_line;
    bool    silent    = state->cur_silent;

    if (startLine + 2 > endLine) return false;
    int32_t nextLine = startLine + 1;

    if (state->sCount[nextLine] < state->blkIndent) return false;
    if (mdit_state_block_is_code_block(state, nextLine)) return false;

    /* Validate the divider line shape (first/second char + lookahead). */
    int32_t pos = state->bMarks[nextLine] + state->tShift[nextLine];
    int32_t end_pos = state->eMarks[nextLine];
    if (pos >= end_pos) return false;
    char first_ch = state->src.data[pos++];
    if (first_ch != '|' && first_ch != '-' && first_ch != ':') return false;
    if (pos >= end_pos) return false;
    char second_ch = state->src.data[pos++];
    bool sec_pipe = (second_ch == '|' || second_ch == '-' || second_ch == ':');
    bool sec_space = (second_ch == ' ' || second_ch == '\t');
    if (!sec_pipe && !sec_space) return false;
    if (first_ch == '-' && sec_space) return false;

    while (pos < end_pos) {
        char ch = state->src.data[pos];
        bool ok = (ch == '|' || ch == '-' || ch == ':' ||
                   ch == ' ' || ch == '\t');
        if (!ok) return false;
        ++pos;
    }

    /* Divider row -> alignments via plain split('|'). */
    mdit_str divider_line = table_get_line(state, startLine + 1);
    table_cells divider;
    table_cells_init(&divider, state->arena);
    /* Plain split on '|' (no escapes). */
    {
        size_t lp = 0;
        for (size_t i = 0; i < divider_line.len; ++i) {
            if (divider_line.data[i] == '|') {
                mdit_str cell = { divider_line.data + lp, i - lp };
                if (!table_cells_push(&divider, cell)) return false;
                lp = i + 1;
            }
        }
        mdit_str tail = { divider_line.data + lp, divider_line.len - lp };
        if (!table_cells_push(&divider, tail)) return false;
    }

    /* Walk divider cells, building the aligns array. */
    table_align_t *aligns = NULL;
    size_t aligns_len = 0;
    aligns = (table_align_t *)mdit_arena_alloc(
        state->arena, divider.len * sizeof *aligns);
    for (size_t i = 0; i < divider.len; ++i) {
        mdit_str t = str_strip(divider.data[i]);
        if (t.len == 0) {
            if (i == 0 || i == divider.len - 1) continue;
            return false;
        }
        if (!table_divider_cell_ok(t)) return false;
        bool starts = (t.data[0] == ':');
        bool ends   = (t.data[t.len - 1] == ':');
        table_align_t a;
        if (ends && starts) a = TABLE_ALIGN_CENTER;
        else if (ends)      a = TABLE_ALIGN_RIGHT;
        else if (starts)    a = TABLE_ALIGN_LEFT;
        else                a = TABLE_ALIGN_NONE;
        aligns[aligns_len++] = a;
    }

    /* Header row -> escapedSplit, drop empty edge cells. */
    mdit_str header_line = str_strip(table_get_line(state, startLine));
    if (!table_contains_byte(header_line, '|')) return false;
    if (mdit_state_block_is_code_block(state, startLine)) return false;

    table_cells header;
    table_cells_init(&header, state->arena);
    if (!table_escaped_split(state->arena, header_line, &header)) return false;
    /* Drop leading empty cell. */
    size_t hstart = 0, hend = header.len;
    if (hend > hstart && header.data[hstart].len == 0) ++hstart;
    if (hend > hstart && header.data[hend - 1].len == 0) --hend;
    size_t column_count = hend - hstart;
    if (column_count == 0 || column_count != aligns_len) return false;

    if (silent) return true;

    mdit_str old_parent = state->parent_type;
    state->parent_type = MDIT_STR_LIT("table");

    size_t n_term = 0;
    const mdit_rule_entry *terminators =
        mdit_ruler_get_rules(state->md->block.ruler,
                             MDIT_STR_LIT("blockquote"), &n_term);

    mdit_token *table_open = mdit_state_block_push(state,
        MDIT_STR_LIT("table_open"), MDIT_STR_LIT("table"), 1);
    if (table_open == NULL) goto done_restore;
    size_t table_open_idx = state->tokens->len - 1;
    mdit_token_set_map(table_open, startLine, 0);

    mdit_token *thead_open = mdit_state_block_push(state,
        MDIT_STR_LIT("thead_open"), MDIT_STR_LIT("thead"), 1);
    if (thead_open == NULL) goto done_restore;
    mdit_token_set_map(thead_open, startLine, startLine + 1);

    mdit_token *tr_open = mdit_state_block_push(state,
        MDIT_STR_LIT("tr_open"), MDIT_STR_LIT("tr"), 1);
    if (tr_open == NULL) goto done_restore;
    mdit_token_set_map(tr_open, startLine, startLine + 1);

    for (size_t i = 0; i < column_count; ++i) {
        mdit_token *th_open = mdit_state_block_push(state,
            MDIT_STR_LIT("th_open"), MDIT_STR_LIT("th"), 1);
        if (th_open == NULL) goto done_restore;
        const char *style = table_align_style(aligns[i]);
        if (style != NULL) {
            (void)mdit_token_attr_set_z(th_open, "style",
                                        mdit_value_cstr(style));
        }
        mdit_token *inl = mdit_state_block_push(state,
            MDIT_STR_LIT("inline"), MDIT_STR_LIT(""), 0);
        if (inl == NULL) goto done_restore;
        mdit_token_set_map(inl, startLine, startLine + 1);
        mdit_str cell = str_strip(header.data[hstart + i]);
        if (cell.len > 0) {
            inl->content = arena_copy_str(state->arena, cell.data, cell.len);
        }
        mdit_token_set_children_empty(inl);
        mdit_token *th_close = mdit_state_block_push(state,
            MDIT_STR_LIT("th_close"), MDIT_STR_LIT("th"), -1);
        if (th_close == NULL) goto done_restore;
    }
    mdit_token *tr_close = mdit_state_block_push(state,
        MDIT_STR_LIT("tr_close"), MDIT_STR_LIT("tr"), -1);
    if (tr_close == NULL) goto done_restore;
    mdit_token *thead_close = mdit_state_block_push(state,
        MDIT_STR_LIT("thead_close"), MDIT_STR_LIT("thead"), -1);
    if (thead_close == NULL) goto done_restore;

    int32_t  autocompleted_cells = 0;
    bool     have_tbody = false;
    size_t   tbody_open_idx = 0;
    int32_t  bodyStart = startLine + 2;
    nextLine = bodyStart;

    while (nextLine < endLine) {
        if (state->sCount[nextLine] < state->blkIndent) break;

        bool terminate = false;
        for (size_t i = 0; i < n_term; ++i) {
            mdit_block_rule_fn fn = (mdit_block_rule_fn)terminators[i].fn;
            int32_t ssl = state->cur_start_line, sel = state->cur_end_line;
            bool    sst = state->cur_silent;
            state->cur_start_line = nextLine;
            state->cur_end_line   = endLine;
            state->cur_silent     = true;
            bool matched = fn(state);
            state->cur_start_line = ssl;
            state->cur_end_line   = sel;
            state->cur_silent     = sst;
            if (matched) { terminate = true; break; }
        }
        if (terminate) break;

        mdit_str row_line = str_strip(table_get_line(state, nextLine));
        if (row_line.len == 0) break;
        if (mdit_state_block_is_code_block(state, nextLine)) break;

        table_cells row;
        table_cells_init(&row, state->arena);
        if (!table_escaped_split(state->arena, row_line, &row)) goto done_restore;
        size_t rstart = 0, rend = row.len;
        if (rend > rstart && row.data[rstart].len == 0) ++rstart;
        if (rend > rstart && row.data[rend - 1].len == 0) --rend;
        size_t row_count = rend - rstart;

        autocompleted_cells += (int32_t)column_count - (int32_t)row_count;
        if (autocompleted_cells > MDIT_TABLE_MAX_AUTOCOMPLETED) break;

        if (!have_tbody) {
            mdit_token *tbody_open = mdit_state_block_push(state,
                MDIT_STR_LIT("tbody_open"), MDIT_STR_LIT("tbody"), 1);
            if (tbody_open == NULL) goto done_restore;
            tbody_open_idx = state->tokens->len - 1;
            mdit_token_set_map(tbody_open, bodyStart, 0);
            have_tbody = true;
        }

        mdit_token *row_tr_open = mdit_state_block_push(state,
            MDIT_STR_LIT("tr_open"), MDIT_STR_LIT("tr"), 1);
        if (row_tr_open == NULL) goto done_restore;
        mdit_token_set_map(row_tr_open, nextLine, nextLine + 1);

        for (size_t i = 0; i < column_count; ++i) {
            mdit_token *td_open = mdit_state_block_push(state,
                MDIT_STR_LIT("td_open"), MDIT_STR_LIT("td"), 1);
            if (td_open == NULL) goto done_restore;
            const char *style = table_align_style(aligns[i]);
            if (style != NULL) {
                (void)mdit_token_attr_set_z(td_open, "style",
                                            mdit_value_cstr(style));
            }
            mdit_token *inl = mdit_state_block_push(state,
                MDIT_STR_LIT("inline"), MDIT_STR_LIT(""), 0);
            if (inl == NULL) goto done_restore;
            mdit_token_set_map(inl, nextLine, nextLine + 1);
            if (i < row_count) {
                mdit_str cell = str_strip(row.data[rstart + i]);
                if (cell.len > 0) {
                    inl->content = arena_copy_str(state->arena,
                                                  cell.data, cell.len);
                }
            }
            mdit_token_set_children_empty(inl);
            mdit_token *td_close = mdit_state_block_push(state,
                MDIT_STR_LIT("td_close"), MDIT_STR_LIT("td"), -1);
            if (td_close == NULL) goto done_restore;
        }
        mdit_token *row_tr_close = mdit_state_block_push(state,
            MDIT_STR_LIT("tr_close"), MDIT_STR_LIT("tr"), -1);
        if (row_tr_close == NULL) goto done_restore;
        ++nextLine;
    }

    if (have_tbody) {
        mdit_token *tbody_close = mdit_state_block_push(state,
            MDIT_STR_LIT("tbody_close"), MDIT_STR_LIT("tbody"), -1);
        if (tbody_close == NULL) goto done_restore;
        state->tokens->data[tbody_open_idx].map.end = nextLine;
    }
    mdit_token *table_close = mdit_state_block_push(state,
        MDIT_STR_LIT("table_close"), MDIT_STR_LIT("table"), -1);
    if (table_close == NULL) goto done_restore;
    state->tokens->data[table_open_idx].map.end = nextLine;
    state->line = nextLine;

done_restore:
    state->parent_type = old_parent;
    return true;
}

/* ---------------------------------------------------------------------
 * Built-in rule: paragraph (catch-all baseline).
 *
 * Walks lines until an empty line, indentation-collapse boundary, or a
 * paragraph-terminator rule (the "alt" chain) returns true in silent
 * mode. Generates [paragraph_open, inline, paragraph_close] tokens.
 * ------------------------------------------------------------------- */
static bool block_paragraph(mdit_state_block *state)
{
    int32_t startLine = state->cur_start_line;
    int32_t endLine   = state->cur_end_line;
    /* upstream resets endLine to lineMax — see paragraph.py. */
    endLine = state->lineMax;

    int32_t nextLine  = startLine + 1;
    mdit_ruler *ruler = state->md->block.ruler;
    size_t n_term = 0;
    const mdit_rule_entry *terminators =
        mdit_ruler_get_rules(ruler, MDIT_STR_LIT("paragraph"), &n_term);

    mdit_str old_parent = state->parent_type;
    state->parent_type = MDIT_STR_LIT("paragraph");

    while (nextLine < endLine) {
        if (mdit_state_block_is_empty(state, nextLine)) break;
        if (state->sCount[nextLine] - state->blkIndent > 3) {
            ++nextLine;
            continue;
        }
        if (state->sCount[nextLine] < 0) {
            ++nextLine;
            continue;
        }
        bool terminate = false;
        for (size_t i = 0; i < n_term; ++i) {
            mdit_block_rule_fn fn =
                (mdit_block_rule_fn)terminators[i].fn;
            int32_t saved_start  = state->cur_start_line;
            int32_t saved_end    = state->cur_end_line;
            bool    saved_silent = state->cur_silent;
            state->cur_start_line = nextLine;
            state->cur_end_line   = endLine;
            state->cur_silent     = true;
            bool matched = fn(state);
            state->cur_start_line = saved_start;
            state->cur_end_line   = saved_end;
            state->cur_silent     = saved_silent;
            if (matched) { terminate = true; break; }
        }
        if (terminate) break;
        ++nextLine;
    }

    /* Pull lines [startLine, nextLine) out of the source. */
    mdit_buf raw;
    mdit_buf_init(&raw);
    (void)mdit_state_block_get_lines(state, startLine, nextLine,
                                     state->blkIndent, false, &raw);

    /* Strip leading + trailing whitespace ('strip()' equivalent). */
    const char *s = raw.data ? raw.data : "";
    size_t lo = 0, hi = raw.len;
    while (lo < hi) {
        unsigned char c = (unsigned char)s[lo];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
            c == '\v' || c == '\f') ++lo;
        else break;
    }
    while (hi > lo) {
        unsigned char c = (unsigned char)s[hi - 1];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
            c == '\v' || c == '\f') --hi;
        else break;
    }
    size_t content_len = hi - lo;
    char *content = NULL;
    if (content_len > 0) {
        content = (char *)mdit_arena_alloc(state->arena, content_len);
        memcpy(content, s + lo, content_len);
    }
    mdit_buf_destroy(&raw);

    state->line = nextLine;

    mdit_token *open = mdit_state_block_push(state,
        MDIT_STR_LIT("paragraph_open"), MDIT_STR_LIT("p"), 1);
    if (open == NULL) { state->parent_type = old_parent; return false; }
    mdit_token_set_map(open, startLine, state->line);

    mdit_token *inl = mdit_state_block_push(state,
        MDIT_STR_LIT("inline"), MDIT_STR_LIT(""), 0);
    if (inl == NULL) { state->parent_type = old_parent; return false; }
    inl->content.data = content;
    inl->content.len  = content_len;
    mdit_token_set_map(inl, startLine, state->line);
    mdit_token_set_children_empty(inl);

    mdit_token *close = mdit_state_block_push(state,
        MDIT_STR_LIT("paragraph_close"), MDIT_STR_LIT("p"), -1);
    if (close == NULL) { state->parent_type = old_parent; return false; }

    state->parent_type = old_parent;
    return true;
}

/* ---------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------- */
/* Static alt-chain arrays. MSVC `/permissive-` rejects compound
 * literals as static initializers, so we use plain aggregate-init
 * with explicit lengths. */
static const mdit_str ALT_PRBL[] = {
    { "paragraph",  9 },
    { "reference", 9 },
    { "blockquote", 10 },
    { "list",       4 },
};
static const mdit_str ALT_PRB[] = {
    { "paragraph",  9 },
    { "reference",  9 },
    { "blockquote", 10 },
};
static const mdit_str ALT_PR[] = {
    { "paragraph", 9 },
    { "reference", 9 },
};

bool mdit_parser_block_init(mdit_parser_block *p, mdit_arena *arena)
{
    p->arena = arena;
    p->ruler = mdit_ruler_new(arena);
    if (p->ruler == NULL) return false;

    typedef struct {
        const char           *name;
        mdit_block_rule_fn    fn;
        const mdit_str       *alt;
        size_t                alt_len;
    } reg;

    reg rules[] = {
        { "table",      block_table,      ALT_PR,   sizeof ALT_PR   / sizeof *ALT_PR   },
        { "code",       block_code,       NULL,     0 },
        { "fence",      block_fence,      ALT_PRBL, sizeof ALT_PRBL / sizeof *ALT_PRBL },
        { "blockquote", block_blockquote, ALT_PRBL, sizeof ALT_PRBL / sizeof *ALT_PRBL },
        { "hr",         block_hr,         ALT_PRBL, sizeof ALT_PRBL / sizeof *ALT_PRBL },
        { "list",       block_list,       ALT_PRB,  sizeof ALT_PRB  / sizeof *ALT_PRB  },
        { "reference",  block_reference,  NULL,     0 },
        { "html_block", block_html_block, ALT_PRB,  sizeof ALT_PRB  / sizeof *ALT_PRB  },
        { "heading",    block_heading,    ALT_PRB,  sizeof ALT_PRB  / sizeof *ALT_PRB  },
        { "lheading",   block_lheading,   NULL,     0 },
        { "paragraph",  block_paragraph,  NULL,     0 },
    };

    for (size_t i = 0; i < sizeof rules / sizeof *rules; ++i) {
        mdit_rule_options opts = { rules[i].alt, rules[i].alt_len };
        mdit_str name = { rules[i].name, strlen(rules[i].name) };
        mdit_rule_status st = mdit_ruler_push(
            p->ruler, name,
            (mdit_rule_fn)rules[i].fn, NULL, opts);
        if (st.index < 0) return false;
    }
    return true;
}

void mdit_parser_block_destroy(mdit_parser_block *p)
{
    mdit_ruler_destroy(p->ruler);
    p->ruler = NULL;
}

/* ---------------------------------------------------------------------
 * Tokenize
 * ------------------------------------------------------------------- */
void mdit_parser_block_tokenize(mdit_parser_block *p,
                                mdit_state_block *state,
                                int32_t start_line,
                                int32_t end_line)
{
    size_t n_rules = 0;
    const mdit_rule_entry *rules =
        mdit_ruler_get_rules(p->ruler, MDIT_STR_LIT(""), &n_rules);
    int32_t line = start_line;
    int32_t maxNesting = (int32_t)state->md->options.max_nesting;
    bool hasEmptyLines = false;

    while (line < end_line) {
        line = mdit_state_block_skip_empty_lines(state, line);
        state->line = line;
        if (line >= end_line) break;
        if (state->sCount[line] < state->blkIndent) break;
        if (state->level >= maxNesting) {
            state->line = end_line;
            break;
        }
        for (size_t i = 0; i < n_rules; ++i) {
            mdit_block_rule_fn fn = (mdit_block_rule_fn)rules[i].fn;
            state->cur_start_line = line;
            state->cur_end_line   = end_line;
            state->cur_silent     = false;
            if (fn(state)) break;
        }
        state->tight = !hasEmptyLines;
        line = state->line;
        if ((line - 1) < end_line && mdit_state_block_is_empty(state, line - 1)) {
            hasEmptyLines = true;
        }
        if (line < end_line && mdit_state_block_is_empty(state, line)) {
            hasEmptyLines = true;
            ++line;
            state->line = line;
        }
    }
}

bool mdit_parser_block_parse(mdit_parser_block *p,
                             mdit_str src,
                             struct mdit_md *md,
                             void *env,
                             mdit_vec_token *out_tokens)
{
    if (src.len == 0) return true;
    mdit_state_block state;
    if (!mdit_state_block_init(&state, p->arena, src, md, env, out_tokens)) {
        return false;
    }
    /* Mirror upstream: when the `code` block rule is disabled, indented
     * lines no longer auto-trigger code-block detection, so individual
     * block rules (fence, heading, table, …) get a chance to match
     * indented content. We surface this through the state-level
     * ``code_enabled`` flag that ``mdit_state_block_is_code_block``
     * consults. */
    state.code_enabled = mdit_ruler_is_rule_enabled(p->ruler,
                                                    MDIT_STR_LIT("code"));
    mdit_parser_block_tokenize(p, &state, state.line, state.lineMax);
    return true;
}
