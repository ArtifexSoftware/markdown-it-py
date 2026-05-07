/*
 * main.h — the ``MarkdownIt`` facade.
 *
 * Owns the three parsers (core / block / inline) and the renderer,
 * plus the option struct. The arena field is *borrowed* from the
 * caller of ``mdit_md_init`` so the same arena can serve multiple
 * parses (caller resets between runs to reclaim memory).
 *
 * Phase 2 scope: only the options actually consumed by the registered
 * rules + renderer are carried (``max_nesting``, ``html``, ``xhtml_out``,
 * ``breaks``, ``lang_prefix``). The remaining upstream options will be
 * added alongside the rules that use them.
 */
#ifndef MDIT_SRC_MAIN_H
#define MDIT_SRC_MAIN_H

#include <stdbool.h>
#include <stdint.h>

#include "arena.h"
#include "parser_block.h"
#include "parser_core.h"
#include "parser_inline.h"
#include "renderer.h"
#include "str.h"
#include "token.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mdit_options {
    int32_t   max_nesting;
    bool      html;
    bool      xhtml_out;
    bool      breaks;
    bool      linkify;
    bool      typographer;
    /* GFM-extension: when true, the strikethrough inline rule accepts
     * single-tilde (`~text~`) markers in addition to the canonical
     * double-tilde (`~~text~~`). Mirrors upstream's
     * `options["strikethrough_single_tilde"]`. */
    bool      strikethrough_single_tilde;
    /* GFM-extension: when true, the `list` block rule recognises
     * `[ ]`/`[x]`/`[X]` task checkboxes at item start and stamps a
     * `checked` meta flag plus `task-list-item` / `contains-task-list`
     * classes on the relevant tokens. */
    bool      tasklists;
    /* When false (default) the rendered task checkbox carries
     * `disabled=""`. Setting this to true matches upstream's
     * `tasklists_editable=True`, which omits `disabled` so the
     * checkbox is interactive in the rendered HTML. */
    bool      tasklists_editable;
    /* GFM-extension (markdown-it-py-only): when true, blockquotes whose
     * first content line is `[!NOTE]` / `[!TIP]` / `[!IMPORTANT]` /
     * `[!WARNING]` / `[!CAUTION]` are emitted as alert containers
     * (`<div class="markdown-alert markdown-alert-<kind>">…</div>`)
     * instead of `<blockquote>`. */
    bool      alerts;
    /* Smart-quote replacements indexed by core/smartquotes:
     *   quotes[0] / quotes[1] - opening / closing double quote
     *   quotes[2] / quotes[3] - opening / closing single quote
     * Each entry is a UTF-8 string; defaults to the curly Unicode
     * marks `“ ” ‘ ’` (U+201C, U+201D, U+2018, U+2019). */
    mdit_str  quotes[4];
    mdit_str  lang_prefix;
} mdit_options;

#define MDIT_OPTIONS_DEFAULTS                                       \
    {                                                               \
        100, false, false, false, false, false, false,              \
        false, false, false,                                        \
        {                                                           \
            MDIT_STR_LIT("\xe2\x80\x9c"), /* U+201C  “ */           \
            MDIT_STR_LIT("\xe2\x80\x9d"), /* U+201D  ” */           \
            MDIT_STR_LIT("\xe2\x80\x98"), /* U+2018  ‘ */           \
            MDIT_STR_LIT("\xe2\x80\x99"), /* U+2019  ’ */           \
        },                                                          \
        MDIT_STR_LIT("language-"),                                  \
    }

/* ---------------------------------------------------------------------
 * Linkifier vtable.
 *
 * Mirrors the linkify-it API surface that upstream's `linkify` rules
 * consume. Hosts plug in their own scanner (typically `mdit_linkifier_default`,
 * which ships with the library and handles explicit-scheme URLs and
 * email addresses). When `options.linkify` is true but `md->linkifier`
 * is NULL, the linkify rules silently no-op (this is a pragmatic
 * divergence from upstream Python, which raises ModuleNotFoundError).
 *
 * `match_at_start`/`match_all` return arena-allocated arrays of
 * `mdit_linkify_match`. The `index` / `last_index` are byte offsets
 * into the input string. `schema` is one of "http:", "https:", "ftp:",
 * "mailto:", "//", or "" for fuzzy links.
 * ------------------------------------------------------------------- */
typedef struct mdit_linkify_match {
    mdit_str schema;
    mdit_str url;       /* normalized href (e.g. "http://" prefixed) */
    mdit_str text;      /* display text */
    size_t   index;     /* byte offset into input where match starts */
    size_t   last_index;/* byte offset into input where match ends */
} mdit_linkify_match;

typedef struct mdit_linkifier {
    void *self;
    bool (*pretest)       (void *self, mdit_str text);
    bool (*test)          (void *self, mdit_str text);
    bool (*match_at_start)(void *self, mdit_arena *arena,
                           mdit_str text, mdit_linkify_match *out);
    /* Returns count; *out is set to an arena-allocated array. */
    size_t (*match_all)   (void *self, mdit_arena *arena,
                           mdit_str text, mdit_linkify_match **out);
} mdit_linkifier;

typedef struct mdit_md {
    mdit_arena         *arena;     /* borrowed */
    mdit_options        options;
    mdit_parser_core    core;
    mdit_parser_block   block;
    mdit_parser_inline  inline_p;  /* `inline` is a C keyword */
    mdit_renderer      *renderer;
    /* Optional linkifier. Populated by callers (e.g. via
     * `mdit_md_set_linkifier`); when NULL, the linkify rules are no-ops. */
    const mdit_linkifier *linkifier;
} mdit_md;

/* Initialize a MarkdownIt with default options + built-in rules. */
bool mdit_md_init   (mdit_md *md, mdit_arena *arena);
void mdit_md_destroy(mdit_md *md);

/* Install a linkifier vtable. Pass NULL to clear. */
void mdit_md_set_linkifier(mdit_md *md, const mdit_linkifier *linkifier);

/* Parse ``src`` into ``out_tokens``. The caller-provided vec must be
 * arena-backed (or malloc-backed) and remain owned by the caller. */
bool mdit_md_parse(mdit_md *md, mdit_str src, void *env,
                   mdit_vec_token *out_tokens);

/* Parse + render in one shot. ``out`` is appended to. */
bool mdit_md_render(mdit_md *md, mdit_str src, void *env, mdit_buf *out);

#ifdef __cplusplus
}
#endif

#endif /* MDIT_SRC_MAIN_H */
