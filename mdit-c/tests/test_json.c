/*
 * test_json.c — minimal coverage for the JSON encoder.
 */
#include "mdit_test.h"

#include "json.h"

#include <string.h>

MDIT_TEST(buf_append_grows)
{
    mdit_buf b; mdit_buf_init_default(&b);
    for (int i = 0; i < 200; ++i) {
        MDIT_ASSERT_TRUE(mdit_buf_append(&b, "abc", 3));
    }
    MDIT_ASSERT_EQ_SZ(mdit_buf_len(&b), 600);
    MDIT_ASSERT_EQ_INT(mdit_buf_str(&b)[599], 'c');
    /* NUL terminator. */
    MDIT_ASSERT_EQ_INT(mdit_buf_str(&b)[600], 0);
    mdit_buf_destroy(&b);
}

MDIT_TEST(json_primitives)
{
    mdit_buf b; mdit_buf_init_default(&b);
    mdit_json_emit_null(&b);
    mdit_buf_append_byte(&b, ' ');
    mdit_json_emit_bool(&b, true);
    mdit_buf_append_byte(&b, ' ');
    mdit_json_emit_bool(&b, false);
    mdit_buf_append_byte(&b, ' ');
    mdit_json_emit_int(&b, 0);
    mdit_buf_append_byte(&b, ' ');
    mdit_json_emit_int(&b, -42);
    mdit_buf_append_byte(&b, ' ');
    mdit_json_emit_int(&b, 9223372036854775807LL);
    MDIT_ASSERT_STR_EQ(mdit_buf_str(&b),
        "null true false 0 -42 9223372036854775807");
    mdit_buf_destroy(&b);
}

MDIT_TEST(json_string_basic)
{
    mdit_buf b; mdit_buf_init_default(&b);
    mdit_json_emit_str(&b, "hello", 5);
    MDIT_ASSERT_STR_EQ(mdit_buf_str(&b), "\"hello\"");
    mdit_buf_destroy(&b);
}

MDIT_TEST(json_string_escapes_quotes_and_backslashes)
{
    mdit_buf b; mdit_buf_init_default(&b);
    const char src[] = "a\"b\\c";
    mdit_json_emit_str(&b, src, sizeof src - 1);
    MDIT_ASSERT_STR_EQ(mdit_buf_str(&b), "\"a\\\"b\\\\c\"");
    mdit_buf_destroy(&b);
}

MDIT_TEST(json_string_escapes_control_chars)
{
    mdit_buf b; mdit_buf_init_default(&b);
    const char src[] = "a\nb\tc\rd\bf\fg";
    mdit_json_emit_str(&b, src, sizeof src - 1);
    /* Same shape Python's json.dumps produces. */
    MDIT_ASSERT_STR_EQ(mdit_buf_str(&b), "\"a\\nb\\tc\\rd\\bf\\fg\"");
    mdit_buf_destroy(&b);
}

MDIT_TEST(json_string_escapes_unprintable_as_uXXXX)
{
    mdit_buf b; mdit_buf_init_default(&b);
    const char src[] = { 'x', 0x01, 0x1F, 'y', 0 };
    mdit_json_emit_str(&b, src, 4);
    MDIT_ASSERT_STR_EQ(mdit_buf_str(&b), "\"x\\u0001\\u001fy\"");
    mdit_buf_destroy(&b);
}

MDIT_TEST(json_string_passes_utf8_through)
{
    /* ensure_ascii=False: non-ASCII bytes pass through verbatim. */
    mdit_buf b; mdit_buf_init_default(&b);
    const char src[] = "ä€\xF0\x9F\x98\x80";
    mdit_json_emit_str(&b, src, sizeof src - 1);
    /* The output should contain the same UTF-8 sequence between the
     * leading and trailing quote. */
    const char want[] = "\"\xC3\xA4\xE2\x82\xAC\xF0\x9F\x98\x80\"";
    MDIT_ASSERT_STR_EQ(mdit_buf_str(&b), want);
    mdit_buf_destroy(&b);
}

#define MDIT_TEST_REGISTRY                                                 \
    MDIT_TEST_LIST_ENTRY(buf_append_grows),                                \
    MDIT_TEST_LIST_ENTRY(json_primitives),                                 \
    MDIT_TEST_LIST_ENTRY(json_string_basic),                               \
    MDIT_TEST_LIST_ENTRY(json_string_escapes_quotes_and_backslashes),     \
    MDIT_TEST_LIST_ENTRY(json_string_escapes_control_chars),              \
    MDIT_TEST_LIST_ENTRY(json_string_escapes_unprintable_as_uXXXX),       \
    MDIT_TEST_LIST_ENTRY(json_string_passes_utf8_through)

#include "mdit_test_main.h"
