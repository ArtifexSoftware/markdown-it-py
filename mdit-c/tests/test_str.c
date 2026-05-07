/*
 * test_str.c — UTF-8 codec, str view, and ASCII classifier tests.
 */
#include "mdit_test.h"

#include "str.h"

#include <stdint.h>
#include <string.h>

static void check_decode_roundtrip(uint32_t cp, const char *expected_bytes,
                                   size_t expected_len)
{
    char enc[8] = {0};
    size_t n = mdit_encode(cp, enc, sizeof enc);
    MDIT_ASSERT_EQ_SZ(n, expected_len);
    MDIT_ASSERT_MEM_EQ(enc, expected_bytes, expected_len);

    uint32_t back = 0;
    size_t consumed = mdit_decode_strict(enc, n, &back);
    MDIT_ASSERT_EQ_SZ(consumed, expected_len);
    MDIT_ASSERT_EQ_INT((long long)back, (long long)cp);
}

MDIT_TEST(str_eq_basic)
{
    MDIT_ASSERT_TRUE (mdit_str_eq(MDIT_STR_LIT("abc"), MDIT_STR_LIT("abc")));
    MDIT_ASSERT_FALSE(mdit_str_eq(MDIT_STR_LIT("abc"), MDIT_STR_LIT("abd")));
    MDIT_ASSERT_FALSE(mdit_str_eq(MDIT_STR_LIT("abc"), MDIT_STR_LIT("ab")));
    MDIT_ASSERT_TRUE (mdit_str_eq(MDIT_STR_LIT(""), mdit_str_make("", 0)));
}

MDIT_TEST(str_eq_zero_terminated)
{
    mdit_str s = MDIT_STR_LIT("hello");
    MDIT_ASSERT_TRUE (mdit_str_eq_z(s, "hello"));
    MDIT_ASSERT_FALSE(mdit_str_eq_z(s, "hellos"));
    MDIT_ASSERT_FALSE(mdit_str_eq_z(s, "Hello"));
}

MDIT_TEST(str_eq_ci_ascii_only)
{
    mdit_str s = MDIT_STR_LIT("Hello");
    MDIT_ASSERT_TRUE (mdit_str_eq_ci(s, "HELLO"));
    MDIT_ASSERT_TRUE (mdit_str_eq_ci(s, "hello"));
    MDIT_ASSERT_FALSE(mdit_str_eq_ci(s, "world"));
    /* Non-ASCII fold is *not* applied; this is intentional. */
    mdit_str t = MDIT_STR_LIT("\xC3\x84");      /* Ä */
    MDIT_ASSERT_FALSE(mdit_str_eq_ci(t, "\xC3\xA4")); /* ä, would match if non-ASCII fold */
}

MDIT_TEST(decode_ascii_one_byte)
{
    uint32_t cp = 0;
    MDIT_ASSERT_EQ_SZ(mdit_decode("A", 1, &cp), 1);
    MDIT_ASSERT_EQ_INT(cp, 'A');
    MDIT_ASSERT_EQ_SZ(mdit_decode_strict("A", 1, &cp), 1);
    MDIT_ASSERT_EQ_INT(cp, 'A');
}

MDIT_TEST(decode_two_byte)
{
    /* U+00E9 'é' = C3 A9 */
    const char src[] = "\xC3\xA9";
    uint32_t cp = 0;
    MDIT_ASSERT_EQ_SZ(mdit_decode_strict(src, sizeof src - 1, &cp), 2);
    MDIT_ASSERT_EQ_INT(cp, 0x00E9);
}

MDIT_TEST(decode_three_byte)
{
    /* U+20AC '€' = E2 82 AC */
    const char src[] = "\xE2\x82\xAC";
    uint32_t cp = 0;
    MDIT_ASSERT_EQ_SZ(mdit_decode_strict(src, sizeof src - 1, &cp), 3);
    MDIT_ASSERT_EQ_INT(cp, 0x20AC);
}

MDIT_TEST(decode_four_byte)
{
    /* U+1F600 grinning face = F0 9F 98 80 */
    const char src[] = "\xF0\x9F\x98\x80";
    uint32_t cp = 0;
    MDIT_ASSERT_EQ_SZ(mdit_decode_strict(src, sizeof src - 1, &cp), 4);
    MDIT_ASSERT_EQ_INT(cp, 0x1F600);
}

MDIT_TEST(decode_overlong_replaced)
{
    /* C0 AF would decode to '/' if accepted; spec says reject. */
    const char src[] = "\xC0\xAF";
    uint32_t cp = 0;
    size_t n = mdit_decode(src, sizeof src - 1, &cp);
    MDIT_ASSERT_EQ_SZ(n, 1);
    MDIT_ASSERT_EQ_INT(cp, MDIT_REPLACEMENT_CHAR);

    MDIT_ASSERT_EQ_SZ(mdit_decode_strict(src, sizeof src - 1, &cp), 0);
}

MDIT_TEST(decode_truncated_replaced)
{
    /* Only the lead byte of a 2-byte sequence. */
    const char src[] = "\xC3";
    uint32_t cp = 0;
    size_t n = mdit_decode(src, sizeof src - 1, &cp);
    MDIT_ASSERT_EQ_SZ(n, 1);
    MDIT_ASSERT_EQ_INT(cp, MDIT_REPLACEMENT_CHAR);
}

MDIT_TEST(decode_surrogate_replaced)
{
    /* High surrogate U+D800 would encode to ED A0 80 -- not legal. */
    const char src[] = "\xED\xA0\x80";
    uint32_t cp = 0;
    size_t n = mdit_decode(src, sizeof src - 1, &cp);
    MDIT_ASSERT_EQ_SZ(n, 1);
    MDIT_ASSERT_EQ_INT(cp, MDIT_REPLACEMENT_CHAR);
}

MDIT_TEST(encode_decode_roundtrips)
{
    check_decode_roundtrip(0x0041, "A",                 1);
    check_decode_roundtrip(0x00E9, "\xC3\xA9",          2);
    check_decode_roundtrip(0x20AC, "\xE2\x82\xAC",      3);
    check_decode_roundtrip(0x1F600, "\xF0\x9F\x98\x80", 4);
    check_decode_roundtrip(MDIT_CODEPOINT_MAX,
                            "\xF4\x8F\xBF\xBF",          4);
}

MDIT_TEST(encode_rejects_invalid_codepoints)
{
    char buf[8];
    /* Surrogate halves: not legal UTF-8. */
    MDIT_ASSERT_EQ_SZ(mdit_encode(0xD800, buf, sizeof buf), 0);
    MDIT_ASSERT_EQ_SZ(mdit_encode(0xDFFF, buf, sizeof buf), 0);
    /* Out of range. */
    MDIT_ASSERT_EQ_SZ(mdit_encode(0x110000, buf, sizeof buf), 0);
    /* Insufficient capacity. */
    MDIT_ASSERT_EQ_SZ(mdit_encode(0x20AC, buf, 2), 0);
}

MDIT_TEST(is_utf8_valid_and_invalid)
{
    MDIT_ASSERT_TRUE (mdit_is_utf8("hello", 5));
    MDIT_ASSERT_TRUE (mdit_is_utf8("\xE2\x82\xAC", 3));
    MDIT_ASSERT_FALSE(mdit_is_utf8("\xC3", 1));
    MDIT_ASSERT_FALSE(mdit_is_utf8("\xC0\xAF", 2));
    MDIT_ASSERT_FALSE(mdit_is_utf8("\xED\xA0\x80", 3));
}

MDIT_TEST(count_codepoints_basic)
{
    /* "héllo€" : 1 + 2 + 1 + 1 + 1 + 3 = 9 bytes, 6 codepoints */
    const char src[] = "h\xC3\xA9llo\xE2\x82\xAC";
    MDIT_ASSERT_EQ_SZ(mdit_count_codepoints(src, sizeof src - 1), 6);
    MDIT_ASSERT_EQ_SZ(mdit_count_codepoints("", 0), 0);
}

MDIT_TEST(ascii_classifiers)
{
    MDIT_ASSERT_TRUE (mdit_is_ascii('a'));
    MDIT_ASSERT_FALSE(mdit_is_ascii(0x80));
    MDIT_ASSERT_TRUE (mdit_is_ascii_digit('0'));
    MDIT_ASSERT_FALSE(mdit_is_ascii_digit('a'));
    MDIT_ASSERT_TRUE (mdit_is_ascii_alpha('Z'));
    MDIT_ASSERT_FALSE(mdit_is_ascii_alpha('5'));
    MDIT_ASSERT_TRUE (mdit_is_ascii_alnum('Z'));
    MDIT_ASSERT_TRUE (mdit_is_ascii_alnum('5'));
    MDIT_ASSERT_FALSE(mdit_is_ascii_alnum('_'));
    MDIT_ASSERT_TRUE (mdit_is_ascii_space(' '));
    MDIT_ASSERT_TRUE (mdit_is_ascii_space('\t'));
    MDIT_ASSERT_TRUE (mdit_is_ascii_space('\n'));
    MDIT_ASSERT_FALSE(mdit_is_ascii_space('a'));
}

MDIT_TEST(md_ascii_punct_matches_commonmark_set)
{
    /* The full set per CommonMark §6.1: */
    const char *punct = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
    for (const char *p = punct; *p; ++p) {
        MDIT_ASSERT_TRUE(mdit_is_md_ascii_punct((unsigned char)*p));
    }
    /* Non-punctuation must reject. */
    const char *not_punct = "abcXYZ09 \t\n";
    for (const char *p = not_punct; *p; ++p) {
        MDIT_ASSERT_FALSE(mdit_is_md_ascii_punct((unsigned char)*p));
    }
    /* Out-of-range codepoints must reject. */
    MDIT_ASSERT_FALSE(mdit_is_md_ascii_punct(0x80));
    MDIT_ASSERT_FALSE(mdit_is_md_ascii_punct(0xFFFD));
}

#define MDIT_TEST_REGISTRY                                                 \
    MDIT_TEST_LIST_ENTRY(str_eq_basic),                                    \
    MDIT_TEST_LIST_ENTRY(str_eq_zero_terminated),                          \
    MDIT_TEST_LIST_ENTRY(str_eq_ci_ascii_only),                            \
    MDIT_TEST_LIST_ENTRY(decode_ascii_one_byte),                           \
    MDIT_TEST_LIST_ENTRY(decode_two_byte),                                 \
    MDIT_TEST_LIST_ENTRY(decode_three_byte),                               \
    MDIT_TEST_LIST_ENTRY(decode_four_byte),                                \
    MDIT_TEST_LIST_ENTRY(decode_overlong_replaced),                        \
    MDIT_TEST_LIST_ENTRY(decode_truncated_replaced),                       \
    MDIT_TEST_LIST_ENTRY(decode_surrogate_replaced),                       \
    MDIT_TEST_LIST_ENTRY(encode_decode_roundtrips),                        \
    MDIT_TEST_LIST_ENTRY(encode_rejects_invalid_codepoints),               \
    MDIT_TEST_LIST_ENTRY(is_utf8_valid_and_invalid),                       \
    MDIT_TEST_LIST_ENTRY(count_codepoints_basic),                          \
    MDIT_TEST_LIST_ENTRY(ascii_classifiers),                               \
    MDIT_TEST_LIST_ENTRY(md_ascii_punct_matches_commonmark_set)

#include "mdit_test_main.h"
