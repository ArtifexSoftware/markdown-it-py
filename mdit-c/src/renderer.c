/*
 * renderer.c — token-stream → HTML.
 *
 * Implementation strategy: keep the ``Renderer`` class' invariants
 * 1:1 with upstream so the eventual oracle pass can compare HTML
 * output byte-for-byte. The default rules below are ports of the
 * matching methods in ``markdown_it.renderer.RendererHTML``.
 */
#include "renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "escape.h"
#include "vec.h"

/* ---------------------------------------------------------------------
 * Rule registry
 *
 * A vec of (token_type → rule_fn) pairs is enough — token type counts
 * top out around 30 even with plugins, so linear search beats a hash.
 * ------------------------------------------------------------------- */
typedef struct mdit_render_entry {
    mdit_str           type;
    mdit_render_rule   rule;
} mdit_render_entry;

MDIT_VEC_DECLARE(render_entries, mdit_render_entry)
MDIT_VEC_DEFINE (render_entries, mdit_render_entry)

struct mdit_renderer {
    mdit_lib_ctx            *lib;
    mdit_arena              *arena;
    mdit_vec_render_entries  rules;
};

static mdit_render_rule lookup_rule(const mdit_renderer *r, mdit_str type)
{
    for (size_t i = 0; i < r->rules.len; ++i) {
        if (mdit_str_eq(r->rules.data[i].type, type)) {
            return r->rules.data[i].rule;
        }
    }
    return NULL;
}

bool mdit_renderer_add_rule(mdit_renderer *r,
                            mdit_str token_type,
                            mdit_render_rule rule)
{
    /* Replace if it already exists. */
    for (size_t i = 0; i < r->rules.len; ++i) {
        if (mdit_str_eq(r->rules.data[i].type, token_type)) {
            r->rules.data[i].rule = rule;
            return true;
        }
    }
    mdit_render_entry *slot = mdit_vec_render_entries_emplace(&r->rules);
    if (slot == NULL) return false;
    slot->type = token_type;
    slot->rule = rule;
    return true;
}

/* ---------------------------------------------------------------------
 * Default rules (forward declarations)
 * ------------------------------------------------------------------- */
static bool rule_code_inline (mdit_renderer *, const mdit_token *, size_t, size_t,
                              const mdit_renderer_options *, void *, mdit_buf *);
static bool rule_code_block  (mdit_renderer *, const mdit_token *, size_t, size_t,
                              const mdit_renderer_options *, void *, mdit_buf *);
static bool rule_fence       (mdit_renderer *, const mdit_token *, size_t, size_t,
                              const mdit_renderer_options *, void *, mdit_buf *);
static bool rule_image       (mdit_renderer *, const mdit_token *, size_t, size_t,
                              const mdit_renderer_options *, void *, mdit_buf *);
static bool rule_hardbreak   (mdit_renderer *, const mdit_token *, size_t, size_t,
                              const mdit_renderer_options *, void *, mdit_buf *);
static bool rule_softbreak   (mdit_renderer *, const mdit_token *, size_t, size_t,
                              const mdit_renderer_options *, void *, mdit_buf *);
static bool rule_text        (mdit_renderer *, const mdit_token *, size_t, size_t,
                              const mdit_renderer_options *, void *, mdit_buf *);
static bool rule_list_item_open(mdit_renderer *, const mdit_token *, size_t, size_t,
                                const mdit_renderer_options *, void *, mdit_buf *);
static bool rule_html_block  (mdit_renderer *, const mdit_token *, size_t, size_t,
                              const mdit_renderer_options *, void *, mdit_buf *);
static bool rule_html_inline (mdit_renderer *, const mdit_token *, size_t, size_t,
                              const mdit_renderer_options *, void *, mdit_buf *);

/* ---------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------- */
mdit_renderer *mdit_renderer_new(mdit_lib_ctx *lib, mdit_arena *arena)
{
    mdit_renderer *r =
        (mdit_renderer *)mdit_arena_zalloc(lib, arena, sizeof *r);
    r->lib   = lib;
    r->arena = arena;
    mdit_vec_render_entries_init(&r->rules, lib, arena);

    /* Pre-register the default render rules to match upstream defaults. */
    (void)mdit_renderer_add_rule(r, MDIT_STR_LIT("code_inline"),  rule_code_inline);
    (void)mdit_renderer_add_rule(r, MDIT_STR_LIT("code_block"),   rule_code_block);
    (void)mdit_renderer_add_rule(r, MDIT_STR_LIT("fence"),        rule_fence);
    (void)mdit_renderer_add_rule(r, MDIT_STR_LIT("image"),        rule_image);
    (void)mdit_renderer_add_rule(r, MDIT_STR_LIT("hardbreak"),    rule_hardbreak);
    (void)mdit_renderer_add_rule(r, MDIT_STR_LIT("softbreak"),    rule_softbreak);
    (void)mdit_renderer_add_rule(r, MDIT_STR_LIT("text"),         rule_text);
    (void)mdit_renderer_add_rule(r, MDIT_STR_LIT("html_block"),   rule_html_block);
    (void)mdit_renderer_add_rule(r, MDIT_STR_LIT("html_inline"),  rule_html_inline);
    (void)mdit_renderer_add_rule(r, MDIT_STR_LIT("list_item_open"),
                                 rule_list_item_open);

    return r;
}

void mdit_renderer_destroy(mdit_renderer *r)
{
    if (r == NULL) return;
    mdit_vec_render_entries_destroy(&r->rules);
    r->arena = NULL;
}

/* ---------------------------------------------------------------------
 * renderAttrs
 *
 * Mirrors the static method ``renderAttrs``: emit ` k="v" k="v"`. We
 * only know how to stringify mdit_value kinds {STR, INT, BOOL}; for
 * floats we fall through to %g (matches Python's str()).
 * ------------------------------------------------------------------- */
static bool emit_attr_value(const mdit_value *v, mdit_buf *out)
{
    char tmp[64];
    int  n = 0;
    switch (v->kind) {
        case MDIT_VALUE_NULL: return mdit_buf_append(out, "None", 4);
        case MDIT_VALUE_BOOL:
            return mdit_buf_append(out, v->u.b ? "True" : "False",
                                   v->u.b ? 4 : 5);
        case MDIT_VALUE_INT:
            n = snprintf(tmp, sizeof tmp, "%lld", (long long)v->u.i);
            if (n < 0) return false;
            return mdit_escape_html((mdit_str){ tmp, (size_t)n }, out);
        case MDIT_VALUE_DOUBLE:
            n = snprintf(tmp, sizeof tmp, "%g", v->u.d);
            if (n < 0) return false;
            return mdit_escape_html((mdit_str){ tmp, (size_t)n }, out);
        case MDIT_VALUE_STR:
            return mdit_escape_html(v->u.s, out);
    }
    return false;
}

bool mdit_renderer_render_attrs(const mdit_token *t, mdit_buf *out)
{
    for (size_t i = 0; i < mdit_map_len(&t->attrs); ++i) {
        const mdit_map_entry *e = mdit_map_at(&t->attrs, i);
        if (!mdit_buf_append_byte(out, ' ')) return false;
        if (!mdit_escape_html(e->key, out)) return false;
        if (!mdit_buf_append(out, "=\"", 2)) return false;
        if (!emit_attr_value(&e->value, out)) return false;
        if (!mdit_buf_append_byte(out, '"')) return false;
    }
    return true;
}

/* ---------------------------------------------------------------------
 * renderToken
 * ------------------------------------------------------------------- */
bool mdit_renderer_render_token(mdit_renderer *r,
                                const mdit_token *tokens, size_t n_tokens,
                                size_t idx,
                                const mdit_renderer_options *opts,
                                void *env,
                                mdit_buf *out)
{
    (void)r; (void)env;
    const mdit_token *t = &tokens[idx];

    /* Tight list paragraphs: hidden tokens render as nothing. */
    if (t->hidden) return true;

    /* Newline before block-level open following a hidden paragraph. */
    if (t->block && t->nesting != -1 && idx > 0 && tokens[idx - 1].hidden) {
        if (!mdit_buf_append_byte(out, '\n')) return false;
    }

    /* < or </, then tag */
    if (t->nesting == -1) {
        if (!mdit_buf_append(out, "</", 2)) return false;
    } else {
        if (!mdit_buf_append_byte(out, '<')) return false;
    }
    if (!mdit_buf_append(out, t->tag.data, t->tag.len)) return false;

    if (!mdit_renderer_render_attrs(t, out)) return false;

    if (t->nesting == 0 && opts->xhtmlOut) {
        if (!mdit_buf_append(out, " /", 2)) return false;
    }

    bool needLf = false;
    if (t->block) {
        needLf = true;
        if (t->nesting == 1 && idx + 1 < n_tokens) {
            const mdit_token *nxt = &tokens[idx + 1];
            if (mdit_str_eq_z(nxt->type, "inline") || nxt->hidden) {
                needLf = false;
            } else if (nxt->nesting == -1 && mdit_str_eq(nxt->tag, t->tag)) {
                needLf = false;
            }
        }
    }
    if (needLf) {
        return mdit_buf_append(out, ">\n", 2);
    }
    return mdit_buf_append_byte(out, '>');
}

/* ---------------------------------------------------------------------
 * Top-level driver + render_inline / render_inline_as_text
 * ------------------------------------------------------------------- */
bool mdit_renderer_render(mdit_renderer *r,
                          const mdit_token *tokens, size_t n_tokens,
                          const mdit_renderer_options *opts,
                          void *env,
                          mdit_buf *out)
{
    for (size_t i = 0; i < n_tokens; ++i) {
        const mdit_token *t = &tokens[i];
        if (mdit_str_eq_z(t->type, "inline")) {
            if (t->children != NULL && t->children_len > 0) {
                if (!mdit_renderer_render_inline(r, t->children,
                        t->children_len, opts, env, out)) return false;
            }
            continue;
        }
        mdit_render_rule rule = lookup_rule(r, t->type);
        if (rule != NULL) {
            if (!rule(r, tokens, n_tokens, i, opts, env, out)) return false;
        } else {
            if (!mdit_renderer_render_token(r, tokens, n_tokens, i,
                                             opts, env, out)) return false;
        }
    }
    return true;
}

bool mdit_renderer_render_inline(mdit_renderer *r,
                                 const mdit_token *tokens, size_t n_tokens,
                                 const mdit_renderer_options *opts,
                                 void *env,
                                 mdit_buf *out)
{
    for (size_t i = 0; i < n_tokens; ++i) {
        mdit_render_rule rule = lookup_rule(r, tokens[i].type);
        if (rule != NULL) {
            if (!rule(r, tokens, n_tokens, i, opts, env, out)) return false;
        } else {
            if (!mdit_renderer_render_token(r, tokens, n_tokens, i,
                                             opts, env, out)) return false;
        }
    }
    return true;
}

bool mdit_renderer_render_inline_as_text(mdit_renderer *r,
                                         const mdit_token *tokens,
                                         size_t n_tokens,
                                         const mdit_renderer_options *opts,
                                         void *env,
                                         mdit_buf *out)
{
    for (size_t i = 0; i < n_tokens; ++i) {
        const mdit_token *t = &tokens[i];
        if (mdit_str_eq_z(t->type, "text")) {
            if (!mdit_buf_append(out, t->content.data, t->content.len)) return false;
        } else if (mdit_str_eq_z(t->type, "image")) {
            if (t->children != NULL && t->children_len > 0) {
                if (!mdit_renderer_render_inline_as_text(r,
                        t->children, t->children_len, opts, env, out)) return false;
            }
        } else if (mdit_str_eq_z(t->type, "softbreak")) {
            if (!mdit_buf_append_byte(out, '\n')) return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------------
 * Default rules
 * ------------------------------------------------------------------- */
static bool rule_code_inline(mdit_renderer *r,
                             const mdit_token *tokens, size_t n_tokens,
                             size_t idx,
                             const mdit_renderer_options *opts,
                             void *env, mdit_buf *out)
{
    (void)r; (void)opts; (void)env; (void)n_tokens;
    const mdit_token *t = &tokens[idx];
    if (!mdit_buf_append(out, "<code", 5)) return false;
    if (!mdit_renderer_render_attrs(t, out)) return false;
    if (!mdit_buf_append_byte(out, '>')) return false;
    if (!mdit_escape_html(t->content, out)) return false;
    return mdit_buf_append(out, "</code>", 7);
}

static bool rule_code_block(mdit_renderer *r,
                            const mdit_token *tokens, size_t n_tokens,
                            size_t idx,
                            const mdit_renderer_options *opts,
                            void *env, mdit_buf *out)
{
    (void)r; (void)opts; (void)env; (void)n_tokens;
    const mdit_token *t = &tokens[idx];
    if (!mdit_buf_append(out, "<pre", 4)) return false;
    if (!mdit_renderer_render_attrs(t, out)) return false;
    if (!mdit_buf_append(out, "><code>", 7)) return false;
    if (!mdit_escape_html(t->content, out)) return false;
    return mdit_buf_append(out, "</code></pre>\n", 14);
}

/* Fence — supports info-string-derived language class. We replicate
 * the upstream split-on-whitespace semantics; ``info.split()[0]`` is
 * the language name. */
static bool rule_fence(mdit_renderer *r,
                       const mdit_token *tokens, size_t n_tokens,
                       size_t idx,
                       const mdit_renderer_options *opts,
                       void *env, mdit_buf *out)
{
    (void)r; (void)env; (void)n_tokens;
    const mdit_token *t = &tokens[idx];

    /* Extract first whitespace-delimited token of info as the lang name
     * (post-unescape). */
    mdit_buf info_unescaped; mdit_buf_init(&info_unescaped, r->lib);
    if (!mdit_unescape_all(t->info, &info_unescaped)) {
        mdit_buf_destroy(&info_unescaped);
        return false;
    }
    const char *info_s = mdit_buf_str(&info_unescaped);
    size_t      info_n = mdit_buf_len(&info_unescaped);
    /* trim leading whitespace */
    size_t lo = 0;
    while (lo < info_n &&
           (info_s[lo] == ' ' || info_s[lo] == '\t' ||
            info_s[lo] == '\n' || info_s[lo] == '\r')) ++lo;
    size_t hi = lo;
    while (hi < info_n &&
           !(info_s[hi] == ' ' || info_s[hi] == '\t' ||
             info_s[hi] == '\n' || info_s[hi] == '\r')) ++hi;
    bool has_lang = (hi > lo);

    if (!mdit_buf_append(out, "<pre><code", 10)) {
        mdit_buf_destroy(&info_unescaped);
        return false;
    }
    if (has_lang) {
        if (!mdit_buf_append(out, " class=\"", 8)) goto fence_fail;
        if (!mdit_buf_append(out, opts->langPrefix.data, opts->langPrefix.len))
            goto fence_fail;
        if (!mdit_escape_html((mdit_str){ info_s + lo, hi - lo }, out))
            goto fence_fail;
        if (!mdit_buf_append_byte(out, '"')) goto fence_fail;
    }
    if (!mdit_renderer_render_attrs(t, out)) goto fence_fail;
    if (!mdit_buf_append_byte(out, '>')) goto fence_fail;
    if (!mdit_escape_html(t->content, out)) goto fence_fail;
    if (!mdit_buf_append(out, "</code></pre>\n", 14)) goto fence_fail;
    mdit_buf_destroy(&info_unescaped);
    return true;
fence_fail:
    mdit_buf_destroy(&info_unescaped);
    return false;
}

/* Image — the image renderer is unusual: it overwrites the alt-text
 * attribute with the result of ``renderInlineAsText`` over its
 * children, then defers to renderToken. We can't mutate the const
 * token, but we *do* want to emit the same bytes upstream produces.
 *
 * Simplification: emit the full ``<img>`` directly, with alt computed
 * from children. This bypasses renderAttrs only for the alt slot. */
static bool rule_image(mdit_renderer *r,
                       const mdit_token *tokens, size_t n_tokens,
                       size_t idx,
                       const mdit_renderer_options *opts,
                       void *env, mdit_buf *out)
{
    (void)n_tokens;
    const mdit_token *t = &tokens[idx];
    if (!mdit_buf_append(out, "<img", 4)) return false;

    /* Walk attrs in declaration order; emit as-is, EXCEPT replace
     * the value of "alt" with the computed inline-as-text from
     * children. */
    for (size_t i = 0; i < mdit_map_len(&t->attrs); ++i) {
        const mdit_map_entry *e = mdit_map_at(&t->attrs, i);
        if (!mdit_buf_append_byte(out, ' ')) return false;
        if (!mdit_escape_html(e->key, out)) return false;
        if (!mdit_buf_append(out, "=\"", 2)) return false;
        if (mdit_str_eq_z(e->key, "alt")) {
            mdit_buf alt; mdit_buf_init(&alt, r->lib);
            if (t->children != NULL && t->children_len > 0) {
                if (!mdit_renderer_render_inline_as_text(r,
                        t->children, t->children_len, opts, env, &alt)) {
                    mdit_buf_destroy(&alt);
                    return false;
                }
            }
            if (!mdit_escape_html((mdit_str){ alt.data, alt.len }, out)) {
                mdit_buf_destroy(&alt);
                return false;
            }
            mdit_buf_destroy(&alt);
        } else {
            if (!emit_attr_value(&e->value, out)) return false;
        }
        if (!mdit_buf_append_byte(out, '"')) return false;
    }

    if (opts->xhtmlOut) {
        return mdit_buf_append(out, " />", 3);
    }
    return mdit_buf_append_byte(out, '>');
}

static bool rule_hardbreak(mdit_renderer *r,
                           const mdit_token *tokens, size_t n_tokens,
                           size_t idx,
                           const mdit_renderer_options *opts,
                           void *env, mdit_buf *out)
{
    (void)r; (void)tokens; (void)n_tokens; (void)idx; (void)env;
    return mdit_buf_append(out,
        opts->xhtmlOut ? "<br />\n" : "<br>\n",
        opts->xhtmlOut ? 7 : 5);
}

static bool rule_softbreak(mdit_renderer *r,
                           const mdit_token *tokens, size_t n_tokens,
                           size_t idx,
                           const mdit_renderer_options *opts,
                           void *env, mdit_buf *out)
{
    (void)r; (void)tokens; (void)n_tokens; (void)idx; (void)env;
    if (opts->breaks) {
        return mdit_buf_append(out,
            opts->xhtmlOut ? "<br />\n" : "<br>\n",
            opts->xhtmlOut ? 7 : 5);
    }
    return mdit_buf_append_byte(out, '\n');
}

static bool rule_text(mdit_renderer *r,
                      const mdit_token *tokens, size_t n_tokens,
                      size_t idx,
                      const mdit_renderer_options *opts,
                      void *env, mdit_buf *out)
{
    (void)r; (void)opts; (void)env; (void)n_tokens;
    return mdit_escape_html(tokens[idx].content, out);
}

/* GFM tasklist override for `list_item_open`. The default `renderToken`
 * emits the opening `<li>` (with attrs); when `meta["checked"]` is
 * present we then append the input element matching upstream's
 * `markdown_it.renderer.list_item_open`. The actual `<li>` is
 * delegated by re-entering `mdit_renderer_render_token`. */
static bool rule_list_item_open(mdit_renderer *r,
                                const mdit_token *tokens, size_t n_tokens,
                                size_t idx,
                                const mdit_renderer_options *opts,
                                void *env, mdit_buf *out)
{
    if (!mdit_renderer_render_token(r, tokens, n_tokens, idx, opts,
                                    env, out)) {
        return false;
    }
    const mdit_token *t = &tokens[idx];
    const mdit_value *checked = mdit_map_get_z(&t->meta, "checked");
    if (checked == NULL) return true;

    /* `<input class="task-list-item-checkbox"[ disabled=""] type="checkbox"
     *  [checked=""]> ` — exact byte sequence upstream emits. */
    if (!mdit_buf_append(out,
            "<input class=\"task-list-item-checkbox\"",
            sizeof "<input class=\"task-list-item-checkbox\"" - 1)) {
        return false;
    }
    if (!opts->tasklists_editable) {
        if (!mdit_buf_append(out, " disabled=\"\"",
                             sizeof " disabled=\"\"" - 1)) return false;
    }
    if (!mdit_buf_append(out, " type=\"checkbox\"",
                         sizeof " type=\"checkbox\"" - 1)) return false;
    if (checked->kind == MDIT_VALUE_BOOL && checked->u.b) {
        if (!mdit_buf_append(out, " checked=\"\"",
                             sizeof " checked=\"\"" - 1)) return false;
    }
    return mdit_buf_append(out, "> ", 2);
}

static bool rule_html_block(mdit_renderer *r,
                            const mdit_token *tokens, size_t n_tokens,
                            size_t idx,
                            const mdit_renderer_options *opts,
                            void *env, mdit_buf *out)
{
    (void)r; (void)opts; (void)env; (void)n_tokens;
    /* HTML passes through verbatim. */
    return mdit_buf_append(out, tokens[idx].content.data,
                           tokens[idx].content.len);
}

static bool rule_html_inline(mdit_renderer *r,
                             const mdit_token *tokens, size_t n_tokens,
                             size_t idx,
                             const mdit_renderer_options *opts,
                             void *env, mdit_buf *out)
{
    (void)r; (void)opts; (void)env; (void)n_tokens;
    return mdit_buf_append(out, tokens[idx].content.data,
                           tokens[idx].content.len);
}
