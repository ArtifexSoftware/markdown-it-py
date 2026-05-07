/*
 * test_token.c — Token API + JSON serializer.
 *
 * Each case constructs a token shape in C and asserts that
 * ``mdit_token_to_json`` produces byte-for-byte the same output as
 * Python's ``json.dumps(token.as_dict(as_upstream=True),
 * ensure_ascii=False)`` for the equivalent Python token. The expected
 * strings live in token_vectors.h, generated from real Python via
 * scripts/gen_token_test_vectors.py.
 */
#include "mdit_test.h"

#include "arena.h"
#include "json.h"
#include "map.h"
#include "str.h"
#include "token.h"

#include "token_vectors.h"

#include <string.h>

/* Helper: render a freshly-built token, assert it matches `want`. */
static void check_json(mdit_token *t, const char *want, const char *label)
{
    mdit_buf b; mdit_buf_init(&b);
    bool ok = mdit_token_to_json(t, &b);
    if (!ok) {
        mdit_test_fail(__FILE__, __LINE__, "mdit_token_to_json returned false");
    }
    if (strcmp(mdit_buf_str(&b), want) != 0) {
        char msg[2048];
        (void)snprintf(msg, sizeof msg,
            "JSON mismatch for [%s]\n  got:  %s\n  want: %s",
            label, mdit_buf_str(&b), want);
        mdit_buf_destroy(&b);
        mdit_test_fail(__FILE__, __LINE__, msg);
    }
    mdit_buf_destroy(&b);
}

MDIT_TEST(token_paragraph_open_minimal)
{
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_token *t = mdit_token_new(&a,
        MDIT_STR_LIT("paragraph_open"), MDIT_STR_LIT("p"), 1);
    check_json(t, MDIT_TOKEN_VECTOR_PARAGRAPH_OPEN_MINIMAL,
               "paragraph_open_minimal");
    mdit_arena_destroy(&a);
}

MDIT_TEST(token_link_open_with_attrs_and_map)
{
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_token *t = mdit_token_new(&a,
        MDIT_STR_LIT("link_open"), MDIT_STR_LIT("a"), 1);
    mdit_token_attr_set_z(t, "href",  mdit_value_cstr("/url"));
    mdit_token_attr_set_z(t, "title", mdit_value_cstr("home"));
    mdit_token_set_map(t, 3, 5);
    check_json(t, MDIT_TOKEN_VECTOR_LINK_OPEN_WITH_ATTRS_AND_MAP,
               "link_open_with_attrs_and_map");
    mdit_arena_destroy(&a);
}

MDIT_TEST(token_code_inline_with_content)
{
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_token *t = mdit_token_new(&a,
        MDIT_STR_LIT("code_inline"), MDIT_STR_LIT("code"), 0);
    mdit_token_set_content(t, MDIT_STR_LIT("x = 1"));
    mdit_token_set_markup (t, MDIT_STR_LIT("`"));
    check_json(t, MDIT_TOKEN_VECTOR_CODE_INLINE_WITH_CONTENT,
               "code_inline_with_content");
    mdit_arena_destroy(&a);
}

MDIT_TEST(token_inline_empty_children)
{
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_token *t = mdit_token_new(&a,
        MDIT_STR_LIT("inline"), MDIT_STR_LIT(""), 0);
    mdit_token_set_children_empty(t);
    check_json(t, MDIT_TOKEN_VECTOR_INLINE_EMPTY_CHILDREN,
               "inline_empty_children");
    mdit_arena_destroy(&a);
}

MDIT_TEST(token_inline_with_one_child)
{
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_token *parent = mdit_token_new(&a,
        MDIT_STR_LIT("inline"), MDIT_STR_LIT(""), 0);
    mdit_token *child = mdit_token_push_child(parent,
        MDIT_STR_LIT("text"), MDIT_STR_LIT(""), 0);
    mdit_token_set_content(child, MDIT_STR_LIT("hi"));
    check_json(parent, MDIT_TOKEN_VECTOR_INLINE_WITH_ONE_CHILD,
               "inline_with_one_child");
    mdit_arena_destroy(&a);
}

MDIT_TEST(token_text_utf8)
{
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_token *t = mdit_token_new(&a,
        MDIT_STR_LIT("text"), MDIT_STR_LIT(""), 0);
    /* 'h' + U+00E9 + "llo " + U+20AC */
    const char content[] = "h\xC3\xA9llo \xE2\x82\xAC";
    mdit_token_set_content(t, mdit_str_make(content, sizeof content - 1));
    check_json(t, MDIT_TOKEN_VECTOR_TEXT_UTF8, "text_utf8");
    mdit_arena_destroy(&a);
}

MDIT_TEST(token_text_control_and_quotes)
{
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_token *t = mdit_token_new(&a,
        MDIT_STR_LIT("text"), MDIT_STR_LIT(""), 0);
    /* Mix of control chars and a literal quote / backslash. */
    const char content[] = "a\nb\tc\rd\\e\"f";
    mdit_token_set_content(t, mdit_str_make(content, sizeof content - 1));
    check_json(t, MDIT_TOKEN_VECTOR_TEXT_CONTROL_AND_QUOTES,
               "text_control_and_quotes");
    mdit_arena_destroy(&a);
}

MDIT_TEST(token_hr_block)
{
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_token *t = mdit_token_new(&a,
        MDIT_STR_LIT("hr"), MDIT_STR_LIT("hr"), 0);
    mdit_token_set_markup(t, MDIT_STR_LIT("---"));
    t->block = true;
    mdit_token_set_map(t, 0, 1);
    check_json(t, MDIT_TOKEN_VECTOR_HR_BLOCK, "hr_block");
    mdit_arena_destroy(&a);
}

MDIT_TEST(token_paragraph_open_hidden)
{
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_token *t = mdit_token_new(&a,
        MDIT_STR_LIT("paragraph_open"), MDIT_STR_LIT("p"), 1);
    t->block  = true;
    t->hidden = true;
    check_json(t, MDIT_TOKEN_VECTOR_PARAGRAPH_OPEN_HIDDEN,
               "paragraph_open_hidden");
    mdit_arena_destroy(&a);
}

/* Direct API tests not driven by JSON vectors. */

MDIT_TEST(token_attr_join_appends_with_space)
{
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_token *t = mdit_token_new(&a,
        MDIT_STR_LIT("paragraph_open"), MDIT_STR_LIT("p"), 1);
    mdit_token_attr_set_z(t, "class", mdit_value_cstr("alpha"));
    mdit_token_attr_join(t, MDIT_STR_LIT("class"), MDIT_STR_LIT("beta"));
    mdit_token_attr_join(t, MDIT_STR_LIT("class"), MDIT_STR_LIT("gamma"));
    const mdit_value *v = mdit_token_attr_get_z(t, "class");
    MDIT_ASSERT_NE(v, NULL);
    MDIT_ASSERT_EQ_INT(v->kind, MDIT_VALUE_STR);
    MDIT_ASSERT_TRUE(mdit_str_eq_z(v->u.s, "alpha beta gamma"));
    mdit_arena_destroy(&a);
}

MDIT_TEST(token_attr_join_creates_when_missing)
{
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_token *t = mdit_token_new(&a,
        MDIT_STR_LIT("paragraph_open"), MDIT_STR_LIT("p"), 1);
    mdit_token_attr_join(t, MDIT_STR_LIT("data-x"), MDIT_STR_LIT("foo"));
    const mdit_value *v = mdit_token_attr_get_z(t, "data-x");
    MDIT_ASSERT_NE(v, NULL);
    MDIT_ASSERT_TRUE(mdit_str_eq_z(v->u.s, "foo"));
    mdit_arena_destroy(&a);
}

MDIT_TEST(token_children_grow_and_index)
{
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_token *parent = mdit_token_new(&a,
        MDIT_STR_LIT("inline"), MDIT_STR_LIT(""), 0);
    for (int i = 0; i < 50; ++i) {
        mdit_token *c = mdit_token_push_child(parent,
            MDIT_STR_LIT("text"), MDIT_STR_LIT(""), 0);
        c->level = i;
    }
    MDIT_ASSERT_EQ_SZ(mdit_token_children_len(parent), 50);
    MDIT_ASSERT_EQ_INT(mdit_token_child_at(parent, 7)->level, 7);
    MDIT_ASSERT_EQ_PTR(mdit_token_child_at(parent, 50), NULL);
    mdit_arena_destroy(&a);
}

#define MDIT_TEST_REGISTRY                                                 \
    MDIT_TEST_LIST_ENTRY(token_paragraph_open_minimal),                    \
    MDIT_TEST_LIST_ENTRY(token_link_open_with_attrs_and_map),              \
    MDIT_TEST_LIST_ENTRY(token_code_inline_with_content),                  \
    MDIT_TEST_LIST_ENTRY(token_inline_empty_children),                     \
    MDIT_TEST_LIST_ENTRY(token_inline_with_one_child),                     \
    MDIT_TEST_LIST_ENTRY(token_text_utf8),                                 \
    MDIT_TEST_LIST_ENTRY(token_text_control_and_quotes),                   \
    MDIT_TEST_LIST_ENTRY(token_hr_block),                                  \
    MDIT_TEST_LIST_ENTRY(token_paragraph_open_hidden),                     \
    MDIT_TEST_LIST_ENTRY(token_attr_join_appends_with_space),              \
    MDIT_TEST_LIST_ENTRY(token_attr_join_creates_when_missing),            \
    MDIT_TEST_LIST_ENTRY(token_children_grow_and_index)

#include "mdit_test_main.h"
