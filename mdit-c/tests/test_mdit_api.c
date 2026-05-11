/*
 * test_mdit_api.c — smoke tests for the public mdit.h facade.
 */
#include "mdit_test.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "mdit/mdit.h"

MDIT_TEST(mdit_api_version_string)
{
    const char *v = mdit_version_string();
    MDIT_ASSERT_NE(v, NULL);
    MDIT_ASSERT_TRUE(strstr(v, "mdit-c") != NULL);
}

MDIT_TEST(mdit_api_new_commonmark_render)
{
    mdit_ctx *ctx = mdit_new(NULL, "commonmark");
    MDIT_ASSERT_NE(ctx, NULL);

    char *html = NULL;
    size_t html_len = 0;
    MDIT_ASSERT_EQ_INT(mdit_render(ctx, "# hi\n", SIZE_MAX, &html, &html_len), MDIT_OK);
    MDIT_ASSERT_NE(html, NULL);
    MDIT_ASSERT_EQ_SZ(html_len, strlen(html));
    MDIT_ASSERT_STR_EQ(html, "<h1>hi</h1>\n");

    mdit_free_string(ctx, html);
    mdit_free(ctx);
}

MDIT_TEST(mdit_api_parse_paragraph_tokens)
{
    mdit_ctx *ctx = mdit_new(NULL, "commonmark");
    MDIT_ASSERT_NE(ctx, NULL);

    const mdit_tokens *toks = NULL;
    MDIT_ASSERT_EQ_INT(mdit_parse(ctx, "hello", SIZE_MAX, &toks), MDIT_OK);
    MDIT_ASSERT_NE(toks, NULL);
    MDIT_ASSERT_EQ_SZ(mdit_tokens_len(toks), 3);
    MDIT_ASSERT_NE(mdit_tokens_at(toks, 0), NULL);

    mdit_free(ctx);
}

MDIT_TEST(mdit_api_unknown_preset_rejected)
{
    MDIT_ASSERT_EQ_PTR(mdit_new(NULL, "not-a-preset"), NULL);
}

MDIT_TEST(mdit_api_disable_unknown_rule)
{
    mdit_ctx *ctx = mdit_new(NULL, "commonmark");
    MDIT_ASSERT_NE(ctx, NULL);

    const char *names[] = { "not-a-rule" };
    MDIT_ASSERT_EQ_INT(mdit_disable(ctx, names, 1, 0), MDIT_ERR_UNKNOWN_NAME);

    mdit_free(ctx);
}

#define MDIT_TEST_REGISTRY                                                  \
    MDIT_TEST_LIST_ENTRY(mdit_api_version_string),                          \
    MDIT_TEST_LIST_ENTRY(mdit_api_new_commonmark_render),                   \
    MDIT_TEST_LIST_ENTRY(mdit_api_parse_paragraph_tokens),                  \
    MDIT_TEST_LIST_ENTRY(mdit_api_unknown_preset_rejected),                   \
    MDIT_TEST_LIST_ENTRY(mdit_api_disable_unknown_rule)

#include "mdit_test_main.h"
