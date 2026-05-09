/*
 * parser_core.c — core chain driver + built-in rules.
 *
 * Built-in rules:
 *   - normalize:    replaces \r\n? with \n; replaces \0 with U+FFFD.
 *   - block:        in non-inline mode, drives the block parser.
 *   - inline:       walks the token stream and parses inline content.
 *   - linkify:      post-processes text tokens to detect bare URLs.
 *   - replacements: typographer text replacements (gated on
 *                   `options.typographer`).
 *   - smartquotes:  curly-quote conversion (gated on `options.typographer`).
 *   - text_join:    converts text_special tokens and merges runs.
 */
#include "parser_core.h"

#include <stdint.h>
#include <string.h>

#include "html_re.h"
#include "main.h"
#include "normalize_url.h"
#include "parser_block.h"
#include "parser_inline.h"
#include "str.h"
#include "utf.h"

/* The U+FFFD replacement char in UTF-8. */
static const unsigned char REPLACEMENT_UTF8[] = { 0xEFu, 0xBFu, 0xBDu };

/* ---------------------------------------------------------------------
 * core/normalize
 *
 * Two-pass scan: first count the size of the output (so we allocate
 * once from the arena), then write. \r\n collapses to \n; bare \r
 * becomes \n; \0 becomes U+FFFD (3 bytes).
 * ------------------------------------------------------------------- */
static void core_normalize(mdit_state_core *state)
{
    const char *src = state->src.data;
    size_t      n   = state->src.len;

    /* Pass 1: compute output length. */
    size_t out_len = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\r') {
            ++out_len; /* '\n' */
            if (i + 1 < n && src[i + 1] == '\n') ++i;
        } else if (c == '\0') {
            out_len += 3;
        } else {
            ++out_len;
        }
    }

    /* Allocate fresh buffer (only if anything would change). */
    bool needs_rewrite = (out_len != n);
    if (!needs_rewrite) {
        /* Even if size matches, lone \r needs to become \n. Detect. */
        for (size_t i = 0; i < n; ++i) {
            if (src[i] == '\r' || src[i] == '\0') {
                needs_rewrite = true;
                break;
            }
        }
    }
    if (!needs_rewrite) return;

    char *dst = (char *)mdit_arena_alloc(state->arena, out_len);
    if (dst == NULL) return;
    size_t w = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\r') {
            dst[w++] = '\n';
            if (i + 1 < n && src[i + 1] == '\n') ++i;
        } else if (c == '\0') {
            dst[w++] = (char)REPLACEMENT_UTF8[0];
            dst[w++] = (char)REPLACEMENT_UTF8[1];
            dst[w++] = (char)REPLACEMENT_UTF8[2];
        } else {
            dst[w++] = (char)c;
        }
    }
    state->src.data = dst;
    state->src.len  = out_len;
}

/* ---------------------------------------------------------------------
 * core/block
 * ------------------------------------------------------------------- */
static void core_block(mdit_state_core *state)
{
    if (state->inlineMode) {
        mdit_token *t = mdit_vec_token_emplace(state->tokens);
        if (t == NULL) return;
        mdit_token_init(t, state->arena, MDIT_STR_LIT("inline"),
                        MDIT_STR_LIT(""), 0);
        t->content = state->src;
        mdit_token_set_map(t, 0, 1);
        mdit_token_set_children_empty(t);
        return;
    }
    (void)mdit_parser_block_parse(&state->md->block, state->src,
                                  state->md, state->env, state->tokens);
}

/* ---------------------------------------------------------------------
 * core/inline
 * ------------------------------------------------------------------- */
static void core_inline(mdit_state_core *state)
{
    /* Iterate by index, since the vec may grow during iteration if the
     * inline parser pushes children — but we only modify children of
     * existing tokens (push_child writes into the parent's children
     * array, which lives in a separate slab). The top-level vec is
     * stable. */
    size_t n = state->tokens->len;
    for (size_t i = 0; i < n; ++i) {
        mdit_token *tk = &state->tokens->data[i];
        if (mdit_str_eq_z(tk->type, "inline")) {
            /* Ensure children is non-NULL ('[]') so subsequent
             * serializers see the empty-list state. */
            mdit_token_set_children_empty(tk);
            (void)mdit_parser_inline_parse(&state->md->inline_p,
                                           tk->content, state->md,
                                           state->env, tk);
        }
    }
}

/* ---------------------------------------------------------------------
 * core/linkify
 *
 * Direct port of `rules_core/linkify.py`. Walks every `inline` token's
 * children in reverse, skipping over markdown links (`link_open` ..
 * `link_close`) and HTML link bodies (`<a>` .. `</a>`), and rewrites
 * text tokens that contain bare URLs / emails into link sequences via
 * the registered linkifier. Gated on `options.linkify` AND a non-NULL
 * `md->linkifier`.
 * ------------------------------------------------------------------- */
static bool ensure_children_cap(mdit_token *parent, size_t want)
{
    if (want <= parent->children_cap) return true;
    size_t cap = parent->children_cap < 4 ? 4 : parent->children_cap;
    while (cap < want) cap *= 2;
    mdit_token *fresh = (mdit_token *)mdit_arena_alloc(parent->arena,
        cap * sizeof *fresh);
    if (fresh == NULL) return false;
    if (parent->children_len > 0) {
        memcpy(fresh, parent->children,
               parent->children_len * sizeof *fresh);
    }
    parent->children     = fresh;
    parent->children_cap = cap;
    return true;
}

/* Replace `parent->children[i]` with `inserts[insert_count]` tokens
 * (mirrors upstream `arrayReplaceAt`). */
static bool replace_child_at(mdit_token *parent, size_t i,
                             const mdit_token *inserts, size_t insert_count)
{
    size_t old_n = parent->children_len;
    if (i >= old_n) return false;
    size_t new_n = old_n - 1 + insert_count;
    if (!ensure_children_cap(parent, new_n)) return false;

    /* Shift tail (i+1 .. old_n) → (i + insert_count .. new_n). */
    if (i + 1 < old_n && insert_count != 1) {
        memmove(&parent->children[i + insert_count],
                &parent->children[i + 1],
                (old_n - i - 1) * sizeof(mdit_token));
    }
    if (insert_count > 0) {
        memcpy(&parent->children[i], inserts,
               insert_count * sizeof(mdit_token));
    }
    parent->children_len = new_n;
    return true;
}

static void core_linkify(mdit_state_core *state)
{
    if (!state->md->options.linkify) return;
    const mdit_linkifier *L = state->md->linkifier;
    if (L == NULL) return;

    for (size_t ti = 0; ti < state->tokens->len; ++ti) {
        mdit_token *inl = &state->tokens->data[ti];
        if (!mdit_str_eq_z(inl->type, "inline")) continue;
        if (!L->pretest(L->self, inl->content)) continue;

        int html_link_level = 0;
        /* Walk children in reverse so insertions don't shift indices
         * we still need to visit. */
        if (inl->children_len == 0) continue;
        size_t i = inl->children_len;
        while (i >= 1) {
            --i;
            mdit_token *cur = &inl->children[i];

            if (mdit_str_eq_z(cur->type, "link_close")) {
                /* Skip back over the contents of this markdown link. */
                if (i == 0) break;
                int32_t target_level = cur->level;
                size_t j = i;
                while (j > 0) {
                    --j;
                    if (inl->children[j].level == target_level &&
                        mdit_str_eq_z(inl->children[j].type, "link_open")) {
                        break;
                    }
                }
                i = j;
                continue;
            }

            if (mdit_str_eq_z(cur->type, "html_inline")) {
                if (mdit_html_is_link_open(cur->content) &&
                    html_link_level > 0) {
                    --html_link_level;
                }
                if (mdit_html_is_link_close(cur->content)) {
                    ++html_link_level;
                }
            }
            if (html_link_level > 0) continue;

            if (!mdit_str_eq_z(cur->type, "text")) continue;
            mdit_str text = cur->content;
            if (!L->test(L->self, text)) continue;

            mdit_linkify_match *links = NULL;
            size_t n_links = L->match_all(L->self, state->arena,
                                          text, &links);
            if (n_links == 0) continue;

            /* Forbid a leading match if preceded by a `text_special`
             * (escape sequence) — mirrors upstream. */
            if (links[0].index == 0 && i > 0 &&
                mdit_str_eq_z(inl->children[i - 1].type, "text_special")) {
                ++links;
                --n_links;
                if (n_links == 0) continue;
            }

            int32_t level = cur->level;
            size_t last_pos = 0;
            mdit_token *nodes = (mdit_token *)mdit_arena_alloc(state->arena,
                (4 * n_links + 1) * sizeof *nodes);
            if (nodes == NULL) continue;
            size_t n_nodes = 0;
            int32_t cur_level = level;

            for (size_t k = 0; k < n_links; ++k) {
                mdit_linkify_match *lk = &links[k];
                mdit_str href;
                if (!mdit_normalize_link(state->arena, lk->url, &href)) continue;
                if (!mdit_validate_link(href)) continue;

                if (lk->index > last_pos) {
                    mdit_token *tk = &nodes[n_nodes++];
                    mdit_token_init(tk, state->arena,
                        MDIT_STR_LIT("text"), MDIT_STR_LIT(""), 0);
                    tk->content = (mdit_str){
                        text.data + last_pos, lk->index - last_pos
                    };
                    tk->level = cur_level;
                }

                mdit_token *open = &nodes[n_nodes++];
                mdit_token_init(open, state->arena,
                    MDIT_STR_LIT("link_open"), MDIT_STR_LIT("a"), 1);
                (void)mdit_token_attr_set_z(open, "href",
                                            mdit_value_str(href));
                open->level = cur_level;
                open->markup = MDIT_STR_LIT("linkify");
                open->info   = MDIT_STR_LIT("auto");
                ++cur_level;

                mdit_str url_text;
                /* Text rendering: schemaless and mailto: get
                 * special-cased so the visible label matches upstream
                 * (`HTTP_RE` / `MAILTO_RE` substitution). */
                if (lk->schema.len == 0) {
                    /* schemaless — prepend "http://" then strip after
                     * normalize_link_text. */
                    size_t plen = 7;
                    char *full = (char *)mdit_arena_alloc(state->arena,
                        plen + lk->text.len);
                    memcpy(full, "http://", plen);
                    memcpy(full + plen, lk->text.data, lk->text.len);
                    mdit_str rendered;
                    if (!mdit_normalize_link_text(state->arena,
                            (mdit_str){ full, plen + lk->text.len },
                            &rendered)) {
                        url_text = lk->text;
                    } else {
                        if (rendered.len >= 7 &&
                            memcmp(rendered.data, "http://", 7) == 0) {
                            url_text = (mdit_str){
                                rendered.data + 7, rendered.len - 7
                            };
                        } else {
                            url_text = rendered;
                        }
                    }
                } else {
                    if (!mdit_normalize_link_text(state->arena,
                            lk->text, &url_text)) {
                        url_text = lk->text;
                    }
                }

                mdit_token *txt = &nodes[n_nodes++];
                mdit_token_init(txt, state->arena,
                    MDIT_STR_LIT("text"), MDIT_STR_LIT(""), 0);
                txt->content = url_text;
                txt->level   = cur_level;

                mdit_token *close = &nodes[n_nodes++];
                mdit_token_init(close, state->arena,
                    MDIT_STR_LIT("link_close"), MDIT_STR_LIT("a"), -1);
                --cur_level;
                close->level  = cur_level;
                close->markup = MDIT_STR_LIT("linkify");
                close->info   = MDIT_STR_LIT("auto");

                last_pos = lk->last_index;
            }

            if (last_pos < text.len) {
                mdit_token *tk = &nodes[n_nodes++];
                mdit_token_init(tk, state->arena,
                    MDIT_STR_LIT("text"), MDIT_STR_LIT(""), 0);
                tk->content = (mdit_str){
                    text.data + last_pos, text.len - last_pos
                };
                tk->level = cur_level;
            }

            (void)replace_child_at(inl, i, nodes, n_nodes);
        }
    }
}

/* ---------------------------------------------------------------------
 * core/replacements
 *
 * Direct port of `rules_core/replacements.py`. Two passes over each
 * inline token's children:
 *
 *   1. SCOPED ABBR — `(c)`, `(C)`, `(r)`, `(R)`, `(tm)`, `(TM)` are
 *      replaced with `©`, `®`, `™` (case-insensitive).
 *   2. RARE — sequences like `+-`, `..+`, `??+`, `--`, `---`, `,,+`
 *      are replaced with their typographic equivalents (±, …, ?..,
 *      –, —, ...).
 *
 * Both passes skip text tokens nested inside `<autolink>`-style links
 * (i.e. between `link_open` / `link_close` whose `info == "auto"`).
 *
 * Each transform is applied in sequence to a fresh arena buffer per
 * pass; we never mutate the input view in place.
 * ------------------------------------------------------------------- */

/* `(^|[^-])---(?=[^-]|$)` and friends use Python's `re.MULTILINE`,
 * where `^` / `$` match newline boundaries. Helpers below treat
 * "start of line" as `pos == 0 || prev byte == '\n'`. */

/* Append `[in.data, in.data + len)` to (`*out`, `*out_len`). Caller
 * must have pre-sized the output buffer. */
static void buf_append(char *out, size_t *out_len,
                       const char *src, size_t n)
{
    if (n == 0) return;
    memcpy(out + *out_len, src, n);
    *out_len += n;
}

static void buf_append_byte(char *out, size_t *out_len, unsigned char c)
{
    out[(*out_len)++] = (char)c;
}

/* Allocate an arena buffer sized for the worst-case expansion of all
 * replacement passes. Worst case: `..` (2 bytes) -> `…` (3 bytes), or
 * `--` (2 bytes) -> `–` (3 bytes), so 1.5x is the upper bound. We
 * round up to 2x + 32 for safety. */
static char *repl_alloc_out(mdit_arena *a, size_t in_len)
{
    return (char *)mdit_arena_alloc(a, 2 * in_len + 32);
}

static mdit_str repl_finalize(mdit_arena *a, char *buf, size_t len)
{
    /* Tighten allocation: shrink to actual length so subsequent
     * arena allocations don't waste the slack. */
    char *tight = (char *)mdit_arena_alloc(a, len == 0 ? 1 : len);
    if (tight && len > 0) memcpy(tight, buf, len);
    return (mdit_str){ tight, len };
}

/* SCOPED_ABBR_RE = `\((c|tm|r)\)`, case-insensitive. */
static mdit_str repl_scoped_abbr(mdit_arena *a, mdit_str in)
{
    char *out = repl_alloc_out(a, in.len);
    if (out == NULL) return in;
    size_t w = 0;
    size_t i = 0;
    while (i < in.len) {
        if (in.data[i] == '(' && i + 1 < in.len) {
            unsigned char c1 = (unsigned char)in.data[i + 1];
            unsigned char c1l = (c1 >= 'A' && c1 <= 'Z') ? c1 + 0x20 : c1;
            /* (c) / (r) */
            if ((c1l == 'c' || c1l == 'r') && i + 2 < in.len &&
                in.data[i + 2] == ')') {
                if (c1l == 'c') {
                    buf_append(out, &w, "\xc2\xa9", 2);  /* © */
                } else {
                    buf_append(out, &w, "\xc2\xae", 2);  /* ® */
                }
                i += 3;
                continue;
            }
            /* (tm) */
            if (c1l == 't' && i + 3 < in.len) {
                unsigned char c2 = (unsigned char)in.data[i + 2];
                unsigned char c2l = (c2 >= 'A' && c2 <= 'Z') ? c2 + 0x20 : c2;
                if (c2l == 'm' && in.data[i + 3] == ')') {
                    buf_append(out, &w, "\xe2\x84\xa2", 3);  /* ™ */
                    i += 4;
                    continue;
                }
            }
        }
        buf_append_byte(out, &w, (unsigned char)in.data[i]);
        ++i;
    }
    return repl_finalize(a, out, w);
}

/* PLUS_MINUS_RE = `\+-` -> `±`. */
static mdit_str repl_plus_minus(mdit_arena *a, mdit_str in)
{
    char *out = repl_alloc_out(a, in.len);
    if (out == NULL) return in;
    size_t w = 0;
    for (size_t i = 0; i < in.len; ++i) {
        if (in.data[i] == '+' && i + 1 < in.len && in.data[i + 1] == '-') {
            buf_append(out, &w, "\xc2\xb1", 2);  /* ± */
            ++i;
            continue;
        }
        buf_append_byte(out, &w, (unsigned char)in.data[i]);
    }
    return repl_finalize(a, out, w);
}

/* ELLIPSIS_RE = `\.{2,}` -> `…`. Two or more dots collapse to a single
 * horizontal ellipsis. */
static mdit_str repl_ellipsis(mdit_arena *a, mdit_str in)
{
    char *out = repl_alloc_out(a, in.len);
    if (out == NULL) return in;
    size_t w = 0;
    size_t i = 0;
    while (i < in.len) {
        if (in.data[i] == '.') {
            size_t j = i + 1;
            while (j < in.len && in.data[j] == '.') ++j;
            size_t run = j - i;
            if (run >= 2) {
                buf_append(out, &w, "\xe2\x80\xa6", 3);  /* … */
                i = j;
                continue;
            }
            buf_append_byte(out, &w, '.');
            ++i;
            continue;
        }
        buf_append_byte(out, &w, (unsigned char)in.data[i]);
        ++i;
    }
    return repl_finalize(a, out, w);
}

/* ELLIPSIS_QUESTION_EXCLAMATION_RE = `([?!])…` -> `\1..`. Reverts the
 * ellipsis collapse for `?….` / `!….` sequences. */
static mdit_str repl_excl_ellipsis_revert(mdit_arena *a, mdit_str in)
{
    char *out = repl_alloc_out(a, in.len);
    if (out == NULL) return in;
    size_t w = 0;
    size_t i = 0;
    while (i < in.len) {
        if ((in.data[i] == '?' || in.data[i] == '!') &&
            i + 3 < in.len &&
            (unsigned char)in.data[i + 1] == 0xe2 &&
            (unsigned char)in.data[i + 2] == 0x80 &&
            (unsigned char)in.data[i + 3] == 0xa6) {
            buf_append_byte(out, &w, (unsigned char)in.data[i]);
            buf_append(out, &w, "..", 2);
            i += 4;
            continue;
        }
        buf_append_byte(out, &w, (unsigned char)in.data[i]);
        ++i;
    }
    return repl_finalize(a, out, w);
}

/* QUESTION_EXCLAMATION_RE = `([?!]){4,}` -> `\1\1\1`. A run of 4+ of
 * the *same* `?` or `!` collapses to 3 of that char. */
static mdit_str repl_question_excl_run(mdit_arena *a, mdit_str in)
{
    char *out = repl_alloc_out(a, in.len);
    if (out == NULL) return in;
    size_t w = 0;
    size_t i = 0;
    while (i < in.len) {
        char c = in.data[i];
        if (c == '?' || c == '!') {
            size_t j = i + 1;
            while (j < in.len && in.data[j] == c) ++j;
            size_t run = j - i;
            if (run >= 4) {
                buf_append_byte(out, &w, (unsigned char)c);
                buf_append_byte(out, &w, (unsigned char)c);
                buf_append_byte(out, &w, (unsigned char)c);
                i = j;
                continue;
            }
            for (size_t k = 0; k < run; ++k) {
                buf_append_byte(out, &w, (unsigned char)c);
            }
            i = j;
            continue;
        }
        buf_append_byte(out, &w, (unsigned char)in.data[i]);
        ++i;
    }
    return repl_finalize(a, out, w);
}

/* COMMA_RE = `,{2,}` -> `,`. Run of two or more commas collapses to one. */
static mdit_str repl_comma(mdit_arena *a, mdit_str in)
{
    char *out = repl_alloc_out(a, in.len);
    if (out == NULL) return in;
    size_t w = 0;
    size_t i = 0;
    while (i < in.len) {
        if (in.data[i] == ',') {
            size_t j = i + 1;
            while (j < in.len && in.data[j] == ',') ++j;
            size_t run = j - i;
            if (run >= 2) {
                buf_append_byte(out, &w, ',');
                i = j;
                continue;
            }
            buf_append_byte(out, &w, ',');
            ++i;
            continue;
        }
        buf_append_byte(out, &w, (unsigned char)in.data[i]);
        ++i;
    }
    return repl_finalize(a, out, w);
}

/* Python `re.\s` set, including U+2028 / U+2029 (which markdown-it's
 * own whitespace predicate excludes). */
static bool is_pyre_ws_cp(uint32_t cp)
{
    if (cp == 0x09 || cp == 0x0A || cp == 0x0B ||
        cp == 0x0C || cp == 0x0D) return true;
    if (cp == 0x20)   return true;
    if (cp == 0x85)   return true; /* NEL */
    if (cp == 0xA0)   return true;
    if (cp == 0x1680) return true;
    if (cp >= 0x2000 && cp <= 0x200A) return true;
    if (cp == 0x2028 || cp == 0x2029) return true;
    if (cp == 0x202F) return true;
    if (cp == 0x205F) return true;
    if (cp == 0x3000) return true;
    return false;
}

/* Look at the codepoint immediately preceding byte index `i`, returning
 * its codepoint and the number of bytes back. If `i == 0`, returns
 * length 0 (caller treats as "start of line/string"). */
static size_t cp_prev(const char *s, size_t i, uint32_t *out_cp)
{
    if (i == 0) { *out_cp = 0; return 0; }
    /* Walk back over UTF-8 continuation bytes. */
    size_t back = 1;
    while (back <= i && back < 4 &&
           ((unsigned char)s[i - back] & 0xC0) == 0x80) {
        ++back;
    }
    if (back > i) back = i;
    (void)mdit_decode(s + i - back, back, out_cp);
    return back;
}

/* Codepoint immediately following byte index `i`. */
static size_t cp_next(const char *s, size_t i, size_t n, uint32_t *out_cp)
{
    if (i >= n) { *out_cp = 0; return 0; }
    return mdit_decode(s + i, n - i, out_cp);
}

/* EM_DASH_RE = `(^|[^-])---(?=[^-]|$)`, multiline. The captured `\1`
 * is preserved (start-of-line or non-`-` byte). */
static mdit_str repl_em_dash(mdit_arena *a, mdit_str in)
{
    char *out = repl_alloc_out(a, in.len);
    if (out == NULL) return in;
    size_t w = 0;
    size_t i = 0;
    while (i < in.len) {
        if (i + 2 < in.len &&
            in.data[i]     == '-' &&
            in.data[i + 1] == '-' &&
            in.data[i + 2] == '-') {
            /* Left context: must be start of buf, after `\n`, or any
             * non-`-` byte. */
            bool left_ok;
            if (i == 0) {
                left_ok = true;
            } else {
                left_ok = (in.data[i - 1] != '-');
            }
            /* Right context: must be end of buf, `\n`, or non-`-`. */
            bool right_ok;
            if (i + 3 >= in.len) {
                right_ok = true;
            } else {
                right_ok = (in.data[i + 3] != '-');
            }
            if (left_ok && right_ok) {
                buf_append(out, &w, "\xe2\x80\x94", 3);  /* — */
                i += 3;
                continue;
            }
        }
        buf_append_byte(out, &w, (unsigned char)in.data[i]);
        ++i;
    }
    return repl_finalize(a, out, w);
}

/* EN_DASH_RE = `(^|\s)--(?=\s|$)`, multiline. `\s` matches Python's
 * Unicode whitespace set. The leading `\s` is preserved; the `--` is
 * replaced with `–` (U+2013). */
static mdit_str repl_en_dash(mdit_arena *a, mdit_str in)
{
    char *out = repl_alloc_out(a, in.len);
    if (out == NULL) return in;
    size_t w = 0;
    size_t i = 0;
    while (i < in.len) {
        if (i + 1 < in.len &&
            in.data[i] == '-' && in.data[i + 1] == '-') {
            /* Left context: start of buf, after `\n`, or after any
             * `\s` codepoint (we keep that codepoint). */
            bool left_ok;
            if (i == 0) {
                left_ok = true;
            } else {
                uint32_t cp;
                size_t back = cp_prev(in.data, i, &cp);
                left_ok = (back > 0) && is_pyre_ws_cp(cp);
            }
            /* Right context: end of buf or any `\s` codepoint. */
            bool right_ok;
            if (i + 2 >= in.len) {
                right_ok = true;
            } else {
                uint32_t cp;
                size_t fwd = cp_next(in.data, i + 2, in.len, &cp);
                right_ok = (fwd > 0) && is_pyre_ws_cp(cp);
            }
            if (left_ok && right_ok) {
                buf_append(out, &w, "\xe2\x80\x93", 3);  /* – */
                i += 2;
                continue;
            }
        }
        buf_append_byte(out, &w, (unsigned char)in.data[i]);
        ++i;
    }
    return repl_finalize(a, out, w);
}

/* EN_DASH_INDENT_RE = `(^|[^-\s])--(?=[^-\s]|$)`, multiline. */
static mdit_str repl_en_dash_indent(mdit_arena *a, mdit_str in)
{
    char *out = repl_alloc_out(a, in.len);
    if (out == NULL) return in;
    size_t w = 0;
    size_t i = 0;
    while (i < in.len) {
        if (i + 1 < in.len &&
            in.data[i] == '-' && in.data[i + 1] == '-') {
            bool left_ok;
            if (i == 0) {
                left_ok = true;
            } else {
                uint32_t cp;
                size_t back = cp_prev(in.data, i, &cp);
                left_ok = (back > 0) && cp != '-' && !is_pyre_ws_cp(cp);
            }
            bool right_ok;
            if (i + 2 >= in.len) {
                right_ok = true;
            } else {
                uint32_t cp;
                size_t fwd = cp_next(in.data, i + 2, in.len, &cp);
                right_ok = (fwd > 0) && cp != '-' && !is_pyre_ws_cp(cp);
            }
            if (left_ok && right_ok) {
                buf_append(out, &w, "\xe2\x80\x93", 3);  /* – */
                i += 2;
                continue;
            }
        }
        buf_append_byte(out, &w, (unsigned char)in.data[i]);
        ++i;
    }
    return repl_finalize(a, out, w);
}

/* Cheap content checks mirroring upstream's `SCOPED_ABBR_RE.search` /
 * `RARE_RE.search` short-circuits. */
static bool content_has_scoped_abbr(mdit_str s)
{
    for (size_t i = 0; i + 2 < s.len; ++i) {
        if (s.data[i] != '(') continue;
        unsigned char c1 = (unsigned char)s.data[i + 1];
        unsigned char c1l = (c1 >= 'A' && c1 <= 'Z') ? c1 + 0x20 : c1;
        if (c1l == 'c' || c1l == 'r') {
            if (i + 2 < s.len && s.data[i + 2] == ')') return true;
        } else if (c1l == 't' && i + 3 < s.len) {
            unsigned char c2 = (unsigned char)s.data[i + 2];
            unsigned char c2l = (c2 >= 'A' && c2 <= 'Z') ? c2 + 0x20 : c2;
            if (c2l == 'm' && s.data[i + 3] == ')') return true;
        }
    }
    return false;
}

static bool content_has_rare(mdit_str s)
{
    for (size_t i = 0; i + 1 < s.len; ++i) {
        char c = s.data[i];
        char d = s.data[i + 1];
        if (c == '+' && d == '-') return true;
        if (c == '.' && d == '.') return true;
        if (c == ',' && d == ',') return true;
        if (c == '-' && d == '-') return true;
        if (c == '?' && d == '?' &&
            i + 3 < s.len && s.data[i + 2] == '?' && s.data[i + 3] == '?') {
            return true;
        }
        if (c == '!' && d == '!' &&
            i + 3 < s.len && s.data[i + 2] == '!' && s.data[i + 3] == '!') {
            return true;
        }
    }
    return false;
}

static void replace_scoped(mdit_arena *a, mdit_token *parent)
{
    /* The `inside_autolink` counter mirrors upstream's odd convention
     * (link_open decrements, link_close increments). The net effect
     * is that text directly inside an autolink is skipped; outside
     * is processed. */
    int inside_autolink = 0;
    for (size_t i = 0; i < parent->children_len; ++i) {
        mdit_token *t = &parent->children[i];
        if (mdit_str_eq_z(t->type, "text") && inside_autolink == 0) {
            if (content_has_scoped_abbr(t->content)) {
                t->content = repl_scoped_abbr(a, t->content);
            }
        }
        if (mdit_str_eq_z(t->type, "link_open") &&
            mdit_str_eq_z(t->info, "auto")) {
            --inside_autolink;
        }
        if (mdit_str_eq_z(t->type, "link_close") &&
            mdit_str_eq_z(t->info, "auto")) {
            ++inside_autolink;
        }
    }
}

static void replace_rare(mdit_arena *a, mdit_token *parent)
{
    int inside_autolink = 0;
    for (size_t i = 0; i < parent->children_len; ++i) {
        mdit_token *t = &parent->children[i];
        if (mdit_str_eq_z(t->type, "text") && inside_autolink == 0 &&
            content_has_rare(t->content)) {
            mdit_str c = t->content;
            c = repl_plus_minus(a, c);
            c = repl_ellipsis(a, c);
            c = repl_excl_ellipsis_revert(a, c);
            c = repl_question_excl_run(a, c);
            c = repl_comma(a, c);
            c = repl_em_dash(a, c);
            c = repl_en_dash(a, c);
            c = repl_en_dash_indent(a, c);
            t->content = c;
        }
        if (mdit_str_eq_z(t->type, "link_open") &&
            mdit_str_eq_z(t->info, "auto")) {
            --inside_autolink;
        }
        if (mdit_str_eq_z(t->type, "link_close") &&
            mdit_str_eq_z(t->info, "auto")) {
            ++inside_autolink;
        }
    }
}

static void core_replacements(mdit_state_core *state)
{
    if (!state->md->options.typographer) return;
    for (size_t i = 0; i < state->tokens->len; ++i) {
        mdit_token *inl = &state->tokens->data[i];
        if (!mdit_str_eq_z(inl->type, "inline")) continue;
        if (inl->children == NULL) continue;
        if (content_has_scoped_abbr(inl->content)) {
            replace_scoped(state->arena, inl);
        }
        if (content_has_rare(inl->content)) {
            replace_rare(state->arena, inl);
        }
    }
}

/* ---------------------------------------------------------------------
 * core/smartquotes
 *
 * Direct port of `rules_core/smartquotes.py`. For each `inline` token
 * whose content contains a `'` or `"`, walk its children and convert
 * straight quotes to the `options.quotes[]` set, using a level-aware
 * stack to match openers with closers.
 *
 * The "previous / next character" lookup walks across sibling tokens
 * so that quotes spanning emphasis or code spans still pair up
 * correctly. The lookup ignores whitespace-only tokens (matching
 * upstream's `not tokens[j].content` short-circuit) and stops at
 * `softbreak` / `hardbreak`.
 * ------------------------------------------------------------------- */

/* `str.replace_at(index, ch_str)`: produce a fresh arena buffer where
 * the byte at `index` is replaced with `repl[0..repl_len)`. Returns
 * the number of bytes added (`repl_len - 1`). */
static mdit_str sq_replace_at(mdit_arena *a, mdit_str s,
                              size_t index, mdit_str repl)
{
    size_t total = s.len - 1 + repl.len;
    char *buf = (char *)mdit_arena_alloc(a, total == 0 ? 1 : total);
    if (buf == NULL) return s;
    memcpy(buf, s.data, index);
    if (repl.len > 0) memcpy(buf + index, repl.data, repl.len);
    memcpy(buf + index + repl.len, s.data + index + 1,
           s.len - index - 1);
    return (mdit_str){ buf, total };
}

/* Stack frame for matching open quotes with close quotes. Mirrors
 * upstream's dict `{token, pos, single, level}`. */
typedef struct sq_frame {
    size_t  token_idx;   /* index in parent->children */
    size_t  pos;         /* byte offset within children[token_idx].content */
    bool    is_single;
    int32_t level;
} sq_frame;

/* Find the codepoint immediately to the *left* of token `i` in
 * `parent->children`. Walks earlier siblings, skipping tokens with
 * empty content, and stops at softbreak/hardbreak (returning 0x20
 * as a "space" sentinel). */
static uint32_t sq_lookup_prev_cp(mdit_token *parent, size_t i)
{
    if (i == 0) return 0x20;
    size_t j = i;
    while (j > 0) {
        --j;
        mdit_token *prev = &parent->children[j];
        if (mdit_str_eq_z(prev->type, "softbreak") ||
            mdit_str_eq_z(prev->type, "hardbreak")) return 0x20;
        if (prev->content.len == 0) continue;
        /* charCodeAt(text, len-1) — last codepoint. */
        uint32_t cp;
        (void)cp_prev(prev->content.data, prev->content.len, &cp);
        return cp;
    }
    return 0x20;
}

static uint32_t sq_lookup_next_cp(mdit_token *parent, size_t i)
{
    size_t n = parent->children_len;
    for (size_t j = i + 1; j < n; ++j) {
        mdit_token *next = &parent->children[j];
        if (mdit_str_eq_z(next->type, "softbreak") ||
            mdit_str_eq_z(next->type, "hardbreak")) return 0x20;
        if (next->content.len == 0) continue;
        uint32_t cp;
        (void)cp_next(next->content.data, 0, next->content.len, &cp);
        return cp;
    }
    return 0x20;
}

static void process_quote_inlines(mdit_arena *a,
                                  mdit_token *parent,
                                  mdit_str opts_quotes[4])
{
    size_t n = parent->children_len;
    sq_frame *stack = NULL;
    size_t   stack_len = 0;
    size_t   stack_cap = 0;

    for (size_t i = 0; i < n; ++i) {
        mdit_token *tok = &parent->children[i];
        int32_t this_level = tok->level;

        /* Pop frames whose level is greater than this token's. */
        while (stack_len > 0 && stack[stack_len - 1].level > this_level) {
            --stack_len;
        }

        if (!mdit_str_eq_z(tok->type, "text")) continue;

        size_t pos = 0;
        while (pos < tok->content.len) {
            const char *base = tok->content.data;
            size_t total = tok->content.len;

            /* Find next quote char from `pos`. */
            size_t q = pos;
            while (q < total && base[q] != '\'' && base[q] != '"') ++q;
            if (q >= total) break;

            bool is_single = (base[q] == '\'');
            mdit_str apostrophe = MDIT_STR_LIT("\xe2\x80\x99");  /* ’ */

            /* Previous codepoint. */
            uint32_t last_cp;
            if (q > 0) {
                (void)cp_prev(base, q, &last_cp);
            } else {
                last_cp = sq_lookup_prev_cp(parent, i);
            }

            /* Next codepoint. */
            uint32_t next_cp;
            if (q + 1 < total) {
                (void)cp_next(base, q + 1, total, &next_cp);
            } else {
                next_cp = sq_lookup_next_cp(parent, i);
            }

            bool last_is_punct = (last_cp != 0) &&
                (mdit_is_md_ascii_punct(last_cp) || mdit_is_punct(last_cp));
            bool next_is_punct = (next_cp != 0) &&
                (mdit_is_md_ascii_punct(next_cp) || mdit_is_punct(next_cp));
            bool last_is_ws = (last_cp != 0) && is_pyre_ws_cp(last_cp);
            bool next_is_ws = (next_cp != 0) && is_pyre_ws_cp(next_cp);

            bool can_open  = true;
            bool can_close = true;
            if (next_is_ws) {
                can_open = false;
            } else if (next_is_punct && !(last_is_ws || last_is_punct)) {
                can_open = false;
            }
            if (last_is_ws) {
                can_close = false;
            } else if (last_is_punct && !(next_is_ws || next_is_punct)) {
                can_close = false;
            }

            /* Special case: 1"" — count first quote as an inch. */
            if (next_cp == 0x22 && base[q] == '"') {
                if (last_cp >= 0x30 && last_cp <= 0x39) {
                    can_open = false;
                    can_close = false;
                }
            }

            if (can_open && can_close) {
                /* Quote inside word vs between punctuation. */
                can_open  = last_is_punct;
                can_close = next_is_punct;
            }

            if (!can_open && !can_close) {
                if (is_single) {
                    /* Bare apostrophe in middle of word. */
                    tok->content = sq_replace_at(a, tok->content, q,
                                                 apostrophe);
                    /* `pos` advances past the new (multi-byte) char. */
                    pos = q + apostrophe.len;
                } else {
                    pos = q + 1;
                }
                continue;
            }

            bool resolved_close = false;
            if (can_close) {
                /* Walk the stack from top down looking for a match. */
                size_t j = stack_len;
                while (j > 0) {
                    --j;
                    if (stack[j].level < this_level) break;
                    if (stack[j].is_single == is_single &&
                        stack[j].level == this_level) {
                        sq_frame open_frame = stack[j];
                        mdit_str open_quote, close_quote;
                        if (is_single) {
                            open_quote  = opts_quotes[2];
                            close_quote = opts_quotes[3];
                        } else {
                            open_quote  = opts_quotes[0];
                            close_quote = opts_quotes[1];
                        }
                        /* Replace the closing quote first (in this
                         * token), THEN the opening quote (potentially
                         * in an earlier token). Mirrors upstream's
                         * order so indices in the *opening* token
                         * stay correct. */
                        tok->content = sq_replace_at(a, tok->content,
                                                     q, close_quote);

                        /* If both opener and closer are in the same
                         * token, the opener's `pos` is still valid
                         * because we just replaced a position *after*
                         * it. Otherwise, mutate the opener's token. */
                        if (open_frame.token_idx == i) {
                            tok->content = sq_replace_at(a, tok->content,
                                open_frame.pos, open_quote);
                        } else {
                            mdit_token *op = &parent->children[open_frame.token_idx];
                            op->content = sq_replace_at(a, op->content,
                                open_frame.pos, open_quote);
                        }

                        /* Advance pos past the new closing quote. */
                        pos = q + close_quote.len;
                        if (open_frame.token_idx == i) {
                            /* Account for the bytes inserted by the
                             * earlier opener replacement (which sits
                             * before our `pos`). */
                            pos += open_quote.len - 1;
                        }

                        stack_len = j;  /* drop this frame and above */
                        resolved_close = true;
                        break;
                    }
                }
            }
            if (resolved_close) continue;

            if (can_open) {
                /* Push a new frame. */
                if (stack_len + 1 > stack_cap) {
                    size_t cap = stack_cap < 4 ? 4 : stack_cap * 2;
                    sq_frame *fresh = (sq_frame *)mdit_arena_alloc(a,
                        cap * sizeof *fresh);
                    if (fresh == NULL) return;
                    if (stack_len > 0) {
                        memcpy(fresh, stack, stack_len * sizeof *fresh);
                    }
                    stack = fresh;
                    stack_cap = cap;
                }
                stack[stack_len++] = (sq_frame){ i, q, is_single, this_level };
                pos = q + 1;
            } else if (can_close && is_single) {
                tok->content = sq_replace_at(a, tok->content, q,
                                             apostrophe);
                pos = q + apostrophe.len;
            } else {
                pos = q + 1;
            }
        }
    }
}

static void core_smartquotes(mdit_state_core *state)
{
    if (!state->md->options.typographer) return;
    for (size_t i = 0; i < state->tokens->len; ++i) {
        mdit_token *inl = &state->tokens->data[i];
        if (!mdit_str_eq_z(inl->type, "inline")) continue;
        if (inl->children == NULL) continue;
        /* Cheap pre-check: any `'` or `"` in the inline source? */
        bool has_quote = false;
        for (size_t k = 0; k < inl->content.len; ++k) {
            if (inl->content.data[k] == '\'' || inl->content.data[k] == '"') {
                has_quote = true;
                break;
            }
        }
        if (!has_quote) continue;
        process_quote_inlines(state->arena, inl,
                              state->md->options.quotes);
    }
}

/* ---------------------------------------------------------------------
 * core/text_join
 *
 * Walk every ``inline`` token's children and (a) convert
 * ``text_special`` tokens to ``text`` and (b) merge adjacent ``text``
 * tokens. Mirrors rules_core/text_join.py exactly.
 * ------------------------------------------------------------------- */
static void core_text_join(mdit_state_core *state)
{
    for (size_t i = 0; i < state->tokens->len; ++i) {
        mdit_token *inl = &state->tokens->data[i];
        if (!mdit_str_eq_z(inl->type, "inline")) continue;
        if (inl->children == NULL || inl->children_len == 0) continue;

        size_t n = inl->children_len;
        /* In-place compaction. */
        size_t w = 0;
        for (size_t r = 0; r < n; ++r) {
            mdit_token *c = &inl->children[r];
            if (mdit_str_eq_z(c->type, "text_special")) {
                c->type = MDIT_STR_LIT("text");
            }
            if (w > 0 &&
                mdit_str_eq_z(c->type, "text") &&
                mdit_str_eq_z(inl->children[w - 1].type, "text")) {
                /* Merge with previous text token. */
                mdit_token *prev = &inl->children[w - 1];
                size_t total = prev->content.len + c->content.len;
                char *buf = (char *)mdit_arena_alloc(state->arena, total);
                memcpy(buf, prev->content.data, prev->content.len);
                memcpy(buf + prev->content.len,
                       c->content.data, c->content.len);
                prev->content.data = buf;
                prev->content.len  = total;
            } else {
                if (w != r) inl->children[w] = *c;
                ++w;
            }
        }
        inl->children_len = w;
    }
}

/* ---------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------- */
bool mdit_parser_core_init(mdit_parser_core *p, mdit_arena *arena)
{
    p->arena = arena;
    p->ruler = mdit_ruler_new(arena);
    if (p->ruler == NULL) return false;
    if (mdit_ruler_push(p->ruler, MDIT_STR_LIT("normalize"),
                        (mdit_rule_fn)core_normalize, NULL,
                        MDIT_RULE_OPTIONS_NONE).index < 0) return false;
    if (mdit_ruler_push(p->ruler, MDIT_STR_LIT("block"),
                        (mdit_rule_fn)core_block, NULL,
                        MDIT_RULE_OPTIONS_NONE).index < 0) return false;
    if (mdit_ruler_push(p->ruler, MDIT_STR_LIT("inline"),
                        (mdit_rule_fn)core_inline, NULL,
                        MDIT_RULE_OPTIONS_NONE).index < 0) return false;
    if (mdit_ruler_push(p->ruler, MDIT_STR_LIT("linkify"),
                        (mdit_rule_fn)core_linkify, NULL,
                        MDIT_RULE_OPTIONS_NONE).index < 0) return false;
    if (mdit_ruler_push(p->ruler, MDIT_STR_LIT("replacements"),
                        (mdit_rule_fn)core_replacements, NULL,
                        MDIT_RULE_OPTIONS_NONE).index < 0) return false;
    if (mdit_ruler_push(p->ruler, MDIT_STR_LIT("smartquotes"),
                        (mdit_rule_fn)core_smartquotes, NULL,
                        MDIT_RULE_OPTIONS_NONE).index < 0) return false;
    if (mdit_ruler_push(p->ruler, MDIT_STR_LIT("text_join"),
                        (mdit_rule_fn)core_text_join, NULL,
                        MDIT_RULE_OPTIONS_NONE).index < 0) return false;
    return true;
}

void mdit_parser_core_destroy(mdit_parser_core *p)
{
    mdit_ruler_destroy(p->ruler);
    p->ruler = NULL;
}

void mdit_parser_core_process(mdit_parser_core *p, mdit_state_core *state)
{
    size_t n = 0;
    const mdit_rule_entry *rules =
        mdit_ruler_get_rules(p->ruler, MDIT_STR_LIT(""), &n);
    for (size_t i = 0; i < n; ++i) {
        if (rules[i].is_callback) {
            (void)rules[i].fn(state, rules[i].user);
        } else {
            mdit_core_rule_fn fn = (mdit_core_rule_fn)rules[i].fn;
            fn(state);
        }
    }
}
