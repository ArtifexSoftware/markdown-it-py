/*
 * test_main.c — end-to-end smoke tests for the MarkdownIt facade.
 *
 * Phase 2 baseline: only the normalize/block/inline core rules and
 * the paragraph + text rules are wired in. So the meaningful inputs
 * are:
 *
 *   ""               → empty token stream, empty render output
 *   "hello"          → [paragraph_open, inline("hello"), paragraph_close]
 *                      renders to "<p>hello</p>\n"
 *   "a\r\nb"         → normalize collapses \r\n; tokens come out as if
 *                      the input were "a\nb" (one paragraph with both
 *                      lines).
 *   "a\0b"           → normalize maps NUL to U+FFFD; the renderer
 *                      escapes the bytes verbatim (text rule passes
 *                      bytes through; renderer does HTML escape).
 *
 * The full token-stream oracle from the Python parser will plug in
 * during Phase 2b when we have the rest of the rules registered.
 */
#include "mdit_test.h"

#include "arena.h"
#include "json.h"
#include "main.h"
#include "mdit/mdit.h"
#include "token.h"

#define LIT(s)  ((mdit_str){ (s), sizeof(s) - 1 })

/* Tiny helper to render and assert. */
static void assert_render(mdit_md *md, mdit_str src, const char *expected)
{
    mdit_buf out;
    mdit_buf_init(&out);
    bool ok = mdit_md_render(md, src, NULL, &out);
    if (!ok) {
        mdit_buf_destroy(&out);
        MDIT_FAIL("mdit_md_render returned false");
    }
    /* Buffer is implicitly NUL-terminated (mdit_buf_append_byte writes
     * the trailing NUL when the buffer grows). Use the str view. */
    if (out.len == 0) {
        if (strlen(expected) != 0) {
            char msg[256];
            snprintf(msg, sizeof msg,
                "render produced empty output, expected: %s", expected);
            mdit_buf_destroy(&out);
            MDIT_FAIL(msg);
        }
    } else {
        if (strcmp(out.data, expected) != 0) {
            char msg[1024];
            snprintf(msg, sizeof msg,
                "render mismatch:\n  got:  %s\n  want: %s",
                out.data, expected);
            mdit_buf_destroy(&out);
            MDIT_FAIL(msg);
        }
    }
    mdit_buf_destroy(&out);
}

/* Helper: count tokens of a given type. */
static size_t count_tokens(const mdit_vec_token *v, const char *type)
{
    size_t hits = 0;
    size_t tlen = strlen(type);
    for (size_t i = 0; i < v->len; ++i) {
        if (v->data[i].type.len == tlen &&
            memcmp(v->data[i].type.data, type, tlen) == 0) ++hits;
    }
    return hits;
}

MDIT_TEST(md_init_destroy_roundtrip)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a;
    mdit_arena_init(&a, 0);
    mdit_md md;
    MDIT_ASSERT_TRUE(mdit_md_init(&md, &lib, &a));
    MDIT_ASSERT_NE(md.renderer, NULL);
    mdit_md_destroy(&md);
    mdit_arena_destroy(&lib, &a);
}

MDIT_TEST(md_parse_empty_input_produces_no_tokens)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_md md;   MDIT_ASSERT_TRUE(mdit_md_init(&md, &lib, &a));

    mdit_vec_token tokens;
    mdit_vec_token_init(&tokens, &lib, &a);
    MDIT_ASSERT_TRUE(mdit_md_parse(&md, LIT(""), NULL, &tokens));
    MDIT_ASSERT_EQ_SZ(tokens.len, 0);
    mdit_vec_token_destroy(&tokens);

    mdit_md_destroy(&md);
    mdit_arena_destroy(&lib, &a);
}

MDIT_TEST(md_parse_simple_paragraph)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_md md;   MDIT_ASSERT_TRUE(mdit_md_init(&md, &lib, &a));

    mdit_vec_token tokens;
    mdit_vec_token_init(&tokens, &lib, &a);
    MDIT_ASSERT_TRUE(mdit_md_parse(&md, LIT("hello"), NULL, &tokens));

    /* Expect: paragraph_open, inline, paragraph_close. */
    MDIT_ASSERT_EQ_SZ(tokens.len, 3);
    MDIT_ASSERT_EQ_SZ(count_tokens(&tokens, "paragraph_open"),  1);
    MDIT_ASSERT_EQ_SZ(count_tokens(&tokens, "inline"),          1);
    MDIT_ASSERT_EQ_SZ(count_tokens(&tokens, "paragraph_close"), 1);

    /* The inline token's content should be "hello" and have one text
     * child. */
    mdit_token *inl = &tokens.data[1];
    MDIT_ASSERT_EQ_SZ(inl->content.len, 5);
    MDIT_ASSERT_MEM_EQ(inl->content.data, "hello", 5);
    MDIT_ASSERT_EQ_SZ(mdit_token_children_len(inl), 1);
    mdit_token *child = mdit_token_child_at(inl, 0);
    MDIT_ASSERT_NE(child, NULL);
    MDIT_ASSERT_EQ_SZ(child->type.len, 4);
    MDIT_ASSERT_MEM_EQ(child->type.data, "text", 4);
    MDIT_ASSERT_EQ_SZ(child->content.len, 5);
    MDIT_ASSERT_MEM_EQ(child->content.data, "hello", 5);

    mdit_vec_token_destroy(&tokens);
    mdit_md_destroy(&md);
    mdit_arena_destroy(&lib, &a);
}

MDIT_TEST(md_render_simple_paragraph)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_md md;   MDIT_ASSERT_TRUE(mdit_md_init(&md, &lib, &a));
    assert_render(&md, LIT("hello"), "<p>hello</p>\n");
    assert_render(&md, LIT("hello world"), "<p>hello world</p>\n");
    mdit_md_destroy(&md);
    mdit_arena_destroy(&lib, &a);
}

MDIT_TEST(md_render_empty_input_produces_empty_output)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_md md;   MDIT_ASSERT_TRUE(mdit_md_init(&md, &lib, &a));
    assert_render(&md, LIT(""), "");
    mdit_md_destroy(&md);
    mdit_arena_destroy(&lib, &a);
}

MDIT_TEST(md_render_normalizes_crlf)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_md md;   MDIT_ASSERT_TRUE(mdit_md_init(&md, &lib, &a));
    /* One paragraph spanning two lines — soft break collapses inside
     * <p>. The text rule emits both lines as text bytes; rendering
     * just emits them as-is (no <br/>). */
    assert_render(&md, LIT("a\r\nb"), "<p>a\nb</p>\n");
    assert_render(&md, LIT("a\rb"),    "<p>a\nb</p>\n");
    mdit_md_destroy(&md);
    mdit_arena_destroy(&lib, &a);
}

MDIT_TEST(md_render_normalizes_null_to_replacement)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_md md;   MDIT_ASSERT_TRUE(mdit_md_init(&md, &lib, &a));
    /* Single NUL becomes the 3-byte UTF-8 replacement char. The
     * empty-string concatenation between \xBD and 'b' is required
     * because MSVC otherwise reads \xBDb as one (out-of-range) hex
     * escape sequence. */
    assert_render(&md, LIT("a\0b"), "<p>a\xEF\xBF\xBD" "b</p>\n");
    mdit_md_destroy(&md);
    mdit_arena_destroy(&lib, &a);
}

MDIT_TEST(md_render_two_paragraphs_separated_by_blank_line)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_md md;   MDIT_ASSERT_TRUE(mdit_md_init(&md, &lib, &a));
    assert_render(&md, LIT("foo\n\nbar"),
                  "<p>foo</p>\n<p>bar</p>\n");
    mdit_md_destroy(&md);
    mdit_arena_destroy(&lib, &a);
}

MDIT_TEST(md_render_html_escapes_inline_text)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_md md;   MDIT_ASSERT_TRUE(mdit_md_init(&md, &lib, &a));
    /* <, >, & in plain text get HTML-escaped. */
    assert_render(&md, LIT("a<b&c>d"),
                  "<p>a&lt;b&amp;c&gt;d</p>\n");
    mdit_md_destroy(&md);
    mdit_arena_destroy(&lib, &a);
}

#define MDIT_TEST_REGISTRY                                                  \
    MDIT_TEST_LIST_ENTRY(md_init_destroy_roundtrip),                        \
    MDIT_TEST_LIST_ENTRY(md_parse_empty_input_produces_no_tokens),          \
    MDIT_TEST_LIST_ENTRY(md_parse_simple_paragraph),                        \
    MDIT_TEST_LIST_ENTRY(md_render_simple_paragraph),                       \
    MDIT_TEST_LIST_ENTRY(md_render_empty_input_produces_empty_output),      \
    MDIT_TEST_LIST_ENTRY(md_render_normalizes_crlf),                        \
    MDIT_TEST_LIST_ENTRY(md_render_normalizes_null_to_replacement),         \
    MDIT_TEST_LIST_ENTRY(md_render_two_paragraphs_separated_by_blank_line), \
    MDIT_TEST_LIST_ENTRY(md_render_html_escapes_inline_text)

#include "mdit_test_main.h"
