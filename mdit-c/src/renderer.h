/*
 * renderer.h — token-stream → HTML, mirroring ``markdown_it.renderer``.
 *
 * The renderer maintains a string-keyed map of per-token-type render
 * functions plus a default ``renderToken`` driver. Plugins (or the
 * caller) register custom rules via ``mdit_renderer_add_rule``.
 *
 * Phase 1 scope:
 *   - Renderer registry + default rules for the token types upstream
 *     ships pre-registered (code_inline, code_block, fence, image,
 *     hardbreak, softbreak, text, html_block, html_inline, plus
 *     list_item_open for the task-list extension).
 *   - ``render`` / ``renderToken`` / ``renderInline`` /
 *     ``renderInlineAsText`` / ``renderAttrs``.
 *   - Hand-crafted-token tests; full oracle integration arrives in
 *     Phase 2 alongside the parser.
 */
#ifndef MDIT_SRC_RENDERER_H
#define MDIT_SRC_RENDERER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "json.h"
#include "map.h"
#include "str.h"
#include "token.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mdit_renderer mdit_renderer;

/*
 * Render rule function signature. ``tokens`` is the parent array,
 * ``idx`` the slot to render. The rule must append its output to
 * ``out`` and return true on success.
 *
 * ``options`` carries renderer flags:
 *   xhtmlOut     -> emit `<br />` etc. instead of `<br>`
 *   breaks       -> render '\n' inside paragraphs as `<br>`
 *   langPrefix   -> prefix for the language class on fenced code
 *
 * It is a borrowed view; callers must not mutate it during rendering.
 *
 * ``env`` is an opaque user pointer threaded through every rule so
 * plugins can stash per-render state without touching globals.
 */
typedef struct mdit_renderer_options {
    bool        xhtmlOut;
    bool        breaks;
    /* When false (default) the GFM tasklist checkbox renders with
     * `disabled=""`. Setting this true mirrors upstream's
     * `tasklists_editable=True` and omits the disabled attribute. */
    bool        tasklists_editable;
    mdit_str    langPrefix;
} mdit_renderer_options;

#define MDIT_RENDERER_OPTIONS_DEFAULTS  \
    { false, false, false, MDIT_STR_LIT("language-") }

typedef bool (*mdit_render_rule)(
    mdit_renderer *r,
    const mdit_token *tokens, size_t n_tokens, size_t idx,
    const mdit_renderer_options *opts,
    void *env,
    mdit_buf *out);

/* Lifecycle. */
mdit_renderer *mdit_renderer_new   (mdit_lib_ctx *lib, mdit_arena *arena);
void           mdit_renderer_destroy(mdit_renderer *r);

/* Register / replace a render rule for a token type. */
bool mdit_renderer_add_rule(mdit_renderer *r,
                            mdit_str token_type,
                            mdit_render_rule rule);

/* ---------------------------------------------------------------------
 * Top-level driver
 *
 * ``mdit_renderer_render`` walks ``tokens`` in order, dispatching to
 * registered rules or falling back to ``renderToken``. Output is
 * appended to ``out`` (caller-initialized).
 * ------------------------------------------------------------------- */
bool mdit_renderer_render(mdit_renderer *r,
                          const mdit_token *tokens, size_t n_tokens,
                          const mdit_renderer_options *opts,
                          void *env,
                          mdit_buf *out);

/* Render the children of an ``inline`` token. Mirrors
 * ``Renderer.renderInline``. */
bool mdit_renderer_render_inline(mdit_renderer *r,
                                 const mdit_token *tokens, size_t n_tokens,
                                 const mdit_renderer_options *opts,
                                 void *env,
                                 mdit_buf *out);

/* Render an inline token list as plain text (used for image alt
 * attributes per CommonMark). */
bool mdit_renderer_render_inline_as_text(mdit_renderer *r,
                                         const mdit_token *tokens,
                                         size_t n_tokens,
                                         const mdit_renderer_options *opts,
                                         void *env,
                                         mdit_buf *out);

/* Default token renderer (open/close tag emission). Exposed because
 * custom rules often delegate back to it after tweaking the token. */
bool mdit_renderer_render_token(mdit_renderer *r,
                                const mdit_token *tokens, size_t n_tokens,
                                size_t idx,
                                const mdit_renderer_options *opts,
                                void *env,
                                mdit_buf *out);

/* Render a token's attribute list (` key="value" key2="value2"`). */
bool mdit_renderer_render_attrs(const mdit_token *t, mdit_buf *out);

#ifdef __cplusplus
}
#endif

#endif /* MDIT_SRC_RENDERER_H */
