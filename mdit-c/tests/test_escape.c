/*
 * test_escape.c — escape_html, unescape_all, isValidEntityCode, emit_utf8.
 *
 * Coverage notes:
 *   - escape_html replaces only & < > " (not single quotes).
 *   - unescape_all decodes (a) backslash escapes from the CommonMark
 *     punctuation set, (b) named HTML entities, (c) numeric entity
 *     references in both decimal and hex.
 *   - Invalid entity references must be left verbatim, matching
 *     upstream "if invalid, return the original match".
 */
#include "mdit_test.h"

#include "escape.h"
#include "json.h"
#include "str.h"

#include <string.h>

static void check_escape(const char *in, const char *want)
{
    mdit_buf b; mdit_buf_init_default(&b);
    MDIT_ASSERT_TRUE(mdit_escape_html(
        (mdit_str){ in, strlen(in) }, &b));
    MDIT_ASSERT_STR_EQ(mdit_buf_str(&b), want);
    mdit_buf_destroy(&b);
}

static void check_unescape(const char *in, const char *want)
{
    mdit_buf b; mdit_buf_init_default(&b);
    MDIT_ASSERT_TRUE(mdit_unescape_all(
        (mdit_str){ in, strlen(in) }, &b));
    MDIT_ASSERT_STR_EQ(mdit_buf_str(&b), want);
    mdit_buf_destroy(&b);
}

MDIT_TEST(escape_html_replaces_only_amp_lt_gt_quot)
{
    check_escape("hello", "hello");
    check_escape("<a>", "&lt;a&gt;");
    check_escape("a & b", "a &amp; b");
    check_escape("\"x\"", "&quot;x&quot;");
    /* Single quote is not escaped (matches Python's escapeHtml). */
    check_escape("It's", "It's");
    check_escape("&<>\"", "&amp;&lt;&gt;&quot;");
    check_escape("", "");
}

MDIT_TEST(unescape_all_fast_path_passthrough)
{
    check_unescape("plain text", "plain text");
    check_unescape("digit 123", "digit 123");
    check_unescape("", "");
}

MDIT_TEST(unescape_all_backslash_escapes)
{
    check_unescape("\\!hello", "!hello");
    check_unescape("\\#tag", "#tag");
    check_unescape("a\\.b\\.c", "a.b.c");
    /* Backslash before a non-escapable char passes through. */
    check_unescape("\\a", "\\a");
    check_unescape("\\\\", "\\");
}

MDIT_TEST(unescape_all_named_entities)
{
    check_unescape("&amp;", "&");
    check_unescape("&lt;&gt;", "<>");
    check_unescape("&copy; 2026", "\xC2\xA9 2026");  /* © */
    check_unescape("&zopf;", "\xF0\x9D\x95\xAB");    /* 𝕫 */
    /* Unknown name → preserved. */
    check_unescape("&notarealentity;", "&notarealentity;");
    /* Missing semicolon → preserved. */
    check_unescape("&amp", "&amp");
}

MDIT_TEST(unescape_all_numeric_entities)
{
    check_unescape("&#65;", "A");
    check_unescape("&#x41;", "A");
    check_unescape("&#x20AC;", "\xE2\x82\xAC");      /* € */
    check_unescape("&#128512;", "\xF0\x9F\x98\x80"); /* 😀 */
    /* Invalid codepoint (NUL is forbidden) → preserved. */
    check_unescape("&#0;", "&#0;");
    /* Surrogate → preserved. */
    check_unescape("&#xD800;", "&#xD800;");
    /* Out of range → preserved. */
    check_unescape("&#x110000;", "&#x110000;");
}

MDIT_TEST(unescape_all_mixed)
{
    check_unescape("\\* and &amp; and &#65; and &nbsp;",
                   "* and & and A and \xC2\xA0");
}

MDIT_TEST(is_valid_entity_code_boundaries)
{
    MDIT_ASSERT_FALSE(mdit_is_valid_entity_code(0x00));
    MDIT_ASSERT_FALSE(mdit_is_valid_entity_code(0x08));
    MDIT_ASSERT_TRUE (mdit_is_valid_entity_code(0x09)); /* tab */
    MDIT_ASSERT_TRUE (mdit_is_valid_entity_code(0x0A)); /* LF */
    MDIT_ASSERT_FALSE(mdit_is_valid_entity_code(0x0B));
    MDIT_ASSERT_TRUE (mdit_is_valid_entity_code(0x0C)); /* FF */
    MDIT_ASSERT_TRUE (mdit_is_valid_entity_code(0x0D));
    MDIT_ASSERT_FALSE(mdit_is_valid_entity_code(0x1F));
    MDIT_ASSERT_TRUE (mdit_is_valid_entity_code(0x20));
    MDIT_ASSERT_FALSE(mdit_is_valid_entity_code(0x7F));
    MDIT_ASSERT_FALSE(mdit_is_valid_entity_code(0x9F));
    MDIT_ASSERT_TRUE (mdit_is_valid_entity_code(0xA0));
    MDIT_ASSERT_FALSE(mdit_is_valid_entity_code(0xD800));
    MDIT_ASSERT_FALSE(mdit_is_valid_entity_code(0xDFFF));
    MDIT_ASSERT_FALSE(mdit_is_valid_entity_code(0xFDD0));
    MDIT_ASSERT_FALSE(mdit_is_valid_entity_code(0xFDEF));
    MDIT_ASSERT_TRUE (mdit_is_valid_entity_code(0xFDD0 - 1));
    MDIT_ASSERT_FALSE(mdit_is_valid_entity_code(0xFFFE));
    MDIT_ASSERT_FALSE(mdit_is_valid_entity_code(0xFFFF));
    MDIT_ASSERT_FALSE(mdit_is_valid_entity_code(0x1FFFE));
    MDIT_ASSERT_TRUE (mdit_is_valid_entity_code(0x10FFFD));
    MDIT_ASSERT_FALSE(mdit_is_valid_entity_code(0x110000));
}

MDIT_TEST(emit_utf8_round_trip)
{
    mdit_buf b; mdit_buf_init_default(&b);
    MDIT_ASSERT_TRUE(mdit_emit_utf8('A', &b));
    MDIT_ASSERT_TRUE(mdit_emit_utf8(0x00E9, &b));   /* é */
    MDIT_ASSERT_TRUE(mdit_emit_utf8(0x20AC, &b));   /* € */
    MDIT_ASSERT_TRUE(mdit_emit_utf8(0x1F600, &b));  /* 😀 */
    const char want[] = "A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
    MDIT_ASSERT_STR_EQ(mdit_buf_str(&b), want);
    mdit_buf_destroy(&b);
}

#define MDIT_TEST_REGISTRY                                                 \
    MDIT_TEST_LIST_ENTRY(escape_html_replaces_only_amp_lt_gt_quot),        \
    MDIT_TEST_LIST_ENTRY(unescape_all_fast_path_passthrough),              \
    MDIT_TEST_LIST_ENTRY(unescape_all_backslash_escapes),                  \
    MDIT_TEST_LIST_ENTRY(unescape_all_named_entities),                     \
    MDIT_TEST_LIST_ENTRY(unescape_all_numeric_entities),                   \
    MDIT_TEST_LIST_ENTRY(unescape_all_mixed),                              \
    MDIT_TEST_LIST_ENTRY(is_valid_entity_code_boundaries),                 \
    MDIT_TEST_LIST_ENTRY(emit_utf8_round_trip)

#include "mdit_test_main.h"
