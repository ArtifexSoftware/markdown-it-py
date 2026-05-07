/*
 * test_renderer.c — token-stream → HTML for hand-crafted token shapes.
 *
 * Phase 1 scope: assert that the default rules emit the expected
 * upstream HTML for representative token streams. Full corpus
 * coverage arrives in Phase 2 once we have a parser.
 *
 * Each test builds a small token list and compares the HTML output
 * against the literal Python's ``RendererHTML`` would produce for the
 * same shape.
 */
#include "mdit_test.h"

#include "arena.h"
#include "escape.h"
#include "json.h"
#include "map.h"
#include "renderer.h"
#include "str.h"
#include "token.h"

#include <string.h>

static void render_seq(const mdit_token *tokens, size_t n,
                       const mdit_renderer_options *opts,
                       mdit_buf *out)
{
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_renderer *r = mdit_renderer_new(&a);
    if (!mdit_renderer_render(r, tokens, n, opts, NULL, out)) {
        mdit_arena_destroy(&a);
        mdit_test_fail(__FILE__, __LINE__, "render: OOM");
    }
    mdit_renderer_destroy(r);
    mdit_arena_destroy(&a);
}

static void check_render(const mdit_token *tokens, size_t n,
                         const mdit_renderer_options *opts,
                         const char *want)
{
    mdit_buf b; mdit_buf_init(&b);
    render_seq(tokens, n, opts, &b);
    if (strcmp(mdit_buf_str(&b), want) != 0) {
        char msg[2048];
        (void)snprintf(msg, sizeof msg,
            "render mismatch\n  got:  %s\n  want: %s",
            mdit_buf_str(&b), want);
        mdit_buf_destroy(&b);
        mdit_test_fail(__FILE__, __LINE__, msg);
    }
    mdit_buf_destroy(&b);
}

/* ---------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------- */
MDIT_TEST(renderer_paragraph_with_text)
{
    mdit_arena a; mdit_arena_init(&a, 0);

    mdit_token tokens[3];
    mdit_token_init(&tokens[0], &a,
        MDIT_STR_LIT("paragraph_open"), MDIT_STR_LIT("p"), 1);
    tokens[0].block = true;
    mdit_token_init(&tokens[1], &a,
        MDIT_STR_LIT("inline"), MDIT_STR_LIT(""), 0);
    tokens[1].block = true;
    /* one text child */
    mdit_token *child = mdit_token_push_child(&tokens[1],
        MDIT_STR_LIT("text"), MDIT_STR_LIT(""), 0);
    mdit_token_set_content(child, MDIT_STR_LIT("hello world"));
    mdit_token_init(&tokens[2], &a,
        MDIT_STR_LIT("paragraph_close"), MDIT_STR_LIT("p"), -1);
    tokens[2].block = true;

    mdit_renderer_options opts = MDIT_RENDERER_OPTIONS_DEFAULTS;
    check_render(tokens, 3, &opts, "<p>hello world</p>\n");

    mdit_arena_destroy(&a);
}

MDIT_TEST(renderer_text_escapes_html)
{
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_token t;
    mdit_token_init(&t, &a, MDIT_STR_LIT("text"), MDIT_STR_LIT(""), 0);
    mdit_token_set_content(&t, MDIT_STR_LIT("<a> & \"q\""));
    mdit_renderer_options opts = MDIT_RENDERER_OPTIONS_DEFAULTS;
    check_render(&t, 1, &opts, "&lt;a&gt; &amp; &quot;q&quot;");
    mdit_arena_destroy(&a);
}

MDIT_TEST(renderer_code_inline_escapes_content)
{
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_token t;
    mdit_token_init(&t, &a, MDIT_STR_LIT("code_inline"), MDIT_STR_LIT("code"), 0);
    mdit_token_set_content(&t, MDIT_STR_LIT("a < b && c > d"));
    mdit_renderer_options opts = MDIT_RENDERER_OPTIONS_DEFAULTS;
    check_render(&t, 1, &opts,
                 "<code>a &lt; b &amp;&amp; c &gt; d</code>");
    mdit_arena_destroy(&a);
}

MDIT_TEST(renderer_code_block_emits_pre_code)
{
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_token t;
    mdit_token_init(&t, &a, MDIT_STR_LIT("code_block"), MDIT_STR_LIT("code"), 0);
    mdit_token_set_content(&t, MDIT_STR_LIT("foo\n"));
    mdit_renderer_options opts = MDIT_RENDERER_OPTIONS_DEFAULTS;
    check_render(&t, 1, &opts, "<pre><code>foo\n</code></pre>\n");
    mdit_arena_destroy(&a);
}

MDIT_TEST(renderer_fence_with_lang)
{
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_token t;
    mdit_token_init(&t, &a, MDIT_STR_LIT("fence"), MDIT_STR_LIT("code"), 0);
    mdit_token_set_info(&t, MDIT_STR_LIT("python"));
    mdit_token_set_content(&t, MDIT_STR_LIT("print(1)\n"));
    mdit_renderer_options opts = MDIT_RENDERER_OPTIONS_DEFAULTS;
    check_render(&t, 1, &opts,
        "<pre><code class=\"language-python\">print(1)\n</code></pre>\n");
    mdit_arena_destroy(&a);
}

MDIT_TEST(renderer_fence_no_lang)
{
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_token t;
    mdit_token_init(&t, &a, MDIT_STR_LIT("fence"), MDIT_STR_LIT("code"), 0);
    mdit_token_set_content(&t, MDIT_STR_LIT("plain\n"));
    mdit_renderer_options opts = MDIT_RENDERER_OPTIONS_DEFAULTS;
    check_render(&t, 1, &opts, "<pre><code>plain\n</code></pre>\n");
    mdit_arena_destroy(&a);
}

MDIT_TEST(renderer_softbreak_default_is_newline)
{
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_token t;
    mdit_token_init(&t, &a, MDIT_STR_LIT("softbreak"), MDIT_STR_LIT(""), 0);
    mdit_renderer_options opts = MDIT_RENDERER_OPTIONS_DEFAULTS;
    check_render(&t, 1, &opts, "\n");
    mdit_arena_destroy(&a);
}

MDIT_TEST(renderer_softbreak_with_breaks_emits_br)
{
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_token t;
    mdit_token_init(&t, &a, MDIT_STR_LIT("softbreak"), MDIT_STR_LIT(""), 0);
    mdit_renderer_options opts = MDIT_RENDERER_OPTIONS_DEFAULTS;
    opts.breaks = true;
    check_render(&t, 1, &opts, "<br>\n");
    opts.xhtmlOut = true;
    check_render(&t, 1, &opts, "<br />\n");
    mdit_arena_destroy(&a);
}

MDIT_TEST(renderer_hardbreak)
{
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_token t;
    mdit_token_init(&t, &a, MDIT_STR_LIT("hardbreak"), MDIT_STR_LIT("br"), 0);
    mdit_renderer_options opts = MDIT_RENDERER_OPTIONS_DEFAULTS;
    check_render(&t, 1, &opts, "<br>\n");
    opts.xhtmlOut = true;
    check_render(&t, 1, &opts, "<br />\n");
    mdit_arena_destroy(&a);
}

MDIT_TEST(renderer_html_block_passes_through)
{
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_token t;
    mdit_token_init(&t, &a, MDIT_STR_LIT("html_block"), MDIT_STR_LIT(""), 0);
    mdit_token_set_content(&t, MDIT_STR_LIT("<div>raw</div>\n"));
    mdit_renderer_options opts = MDIT_RENDERER_OPTIONS_DEFAULTS;
    check_render(&t, 1, &opts, "<div>raw</div>\n");
    mdit_arena_destroy(&a);
}

MDIT_TEST(renderer_image_uses_inline_as_text_for_alt)
{
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_token img;
    mdit_token_init(&img, &a,
        MDIT_STR_LIT("image"), MDIT_STR_LIT("img"), 0);
    mdit_token_attr_set_z(&img, "src", mdit_value_cstr("/x.png"));
    mdit_token_attr_set_z(&img, "alt", mdit_value_cstr(""));
    /* alt text is computed from inline children, not the attr value. */
    mdit_token *child = mdit_token_push_child(&img,
        MDIT_STR_LIT("text"), MDIT_STR_LIT(""), 0);
    mdit_token_set_content(child, MDIT_STR_LIT("a < b"));

    mdit_renderer_options opts = MDIT_RENDERER_OPTIONS_DEFAULTS;
    check_render(&img, 1, &opts, "<img src=\"/x.png\" alt=\"a &lt; b\">");

    opts.xhtmlOut = true;
    check_render(&img, 1, &opts, "<img src=\"/x.png\" alt=\"a &lt; b\" />");

    mdit_arena_destroy(&a);
}

MDIT_TEST(renderer_renderToken_self_closing_xhtml)
{
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_token hr;
    mdit_token_init(&hr, &a, MDIT_STR_LIT("hr"), MDIT_STR_LIT("hr"), 0);
    hr.block = true;
    mdit_renderer_options opts = MDIT_RENDERER_OPTIONS_DEFAULTS;
    check_render(&hr, 1, &opts, "<hr>\n");
    opts.xhtmlOut = true;
    check_render(&hr, 1, &opts, "<hr />\n");
    mdit_arena_destroy(&a);
}

/* File-scope custom rule: wraps text content in <span>...</span>. */
static bool custom_text_rule(mdit_renderer *rr,
                             const mdit_token *tokens, size_t n, size_t idx,
                             const mdit_renderer_options *opts,
                             void *env, mdit_buf *out)
{
    (void)rr; (void)n; (void)opts; (void)env;
    if (!mdit_buf_append(out, "<span>", 6)) return false;
    if (!mdit_escape_html(tokens[idx].content, out)) return false;
    return mdit_buf_append(out, "</span>", 7);
}

MDIT_TEST(renderer_custom_rule_replaces_default)
{
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_renderer *r = mdit_renderer_new(&a);
    MDIT_ASSERT_TRUE(mdit_renderer_add_rule(r,
        MDIT_STR_LIT("text"), custom_text_rule));

    mdit_token t;
    mdit_token_init(&t, &a, MDIT_STR_LIT("text"), MDIT_STR_LIT(""), 0);
    mdit_token_set_content(&t, MDIT_STR_LIT("hi & bye"));

    mdit_buf b; mdit_buf_init(&b);
    mdit_renderer_options opts = MDIT_RENDERER_OPTIONS_DEFAULTS;
    MDIT_ASSERT_TRUE(mdit_renderer_render(r, &t, 1, &opts, NULL, &b));
    MDIT_ASSERT_STR_EQ(mdit_buf_str(&b), "<span>hi &amp; bye</span>");
    mdit_buf_destroy(&b);

    mdit_renderer_destroy(r);
    mdit_arena_destroy(&a);
}

/* Render tag attrs in a couple variants. */
MDIT_TEST(renderer_attrs_escape_keys_and_values)
{
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_token t;
    mdit_token_init(&t, &a, MDIT_STR_LIT("a"), MDIT_STR_LIT("a"), 1);
    mdit_token_attr_set_z(&t, "href", mdit_value_cstr("/p?q=1&r=2"));
    mdit_token_attr_set_z(&t, "title", mdit_value_cstr("a \"quoted\" title"));

    mdit_buf b; mdit_buf_init(&b);
    MDIT_ASSERT_TRUE(mdit_renderer_render_attrs(&t, &b));
    MDIT_ASSERT_STR_EQ(mdit_buf_str(&b),
        " href=\"/p?q=1&amp;r=2\" title=\"a &quot;quoted&quot; title\"");
    mdit_buf_destroy(&b);

    mdit_arena_destroy(&a);
}

#define MDIT_TEST_REGISTRY                                                 \
    MDIT_TEST_LIST_ENTRY(renderer_paragraph_with_text),                    \
    MDIT_TEST_LIST_ENTRY(renderer_text_escapes_html),                      \
    MDIT_TEST_LIST_ENTRY(renderer_code_inline_escapes_content),            \
    MDIT_TEST_LIST_ENTRY(renderer_code_block_emits_pre_code),              \
    MDIT_TEST_LIST_ENTRY(renderer_fence_with_lang),                        \
    MDIT_TEST_LIST_ENTRY(renderer_fence_no_lang),                          \
    MDIT_TEST_LIST_ENTRY(renderer_softbreak_default_is_newline),           \
    MDIT_TEST_LIST_ENTRY(renderer_softbreak_with_breaks_emits_br),         \
    MDIT_TEST_LIST_ENTRY(renderer_hardbreak),                              \
    MDIT_TEST_LIST_ENTRY(renderer_html_block_passes_through),              \
    MDIT_TEST_LIST_ENTRY(renderer_image_uses_inline_as_text_for_alt),      \
    MDIT_TEST_LIST_ENTRY(renderer_renderToken_self_closing_xhtml),         \
    MDIT_TEST_LIST_ENTRY(renderer_custom_rule_replaces_default),           \
    MDIT_TEST_LIST_ENTRY(renderer_attrs_escape_keys_and_values)

#include "mdit_test_main.h"
