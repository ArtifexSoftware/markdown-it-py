/*
 * test_utf.c — Unicode classification tests.
 *
 * Two layers of coverage:
 *   1. Hand-picked codepoints covering each category boundary and
 *      every entry of the explicit whitespace set.
 *   2. The auto-generated cross-check vector at tests/utf_vectors.h,
 *      which carries `is_punct` / `is_whitespace` answers from the
 *      Python interpreter for ~8500 sampled codepoints. Drift between
 *      Python and C fails the build immediately.
 */
#include "mdit_test.h"

#include "utf.h"

#include "utf_vectors.h"

MDIT_TEST(whitespace_explicit_set)
{
    /* The full markdown-it whitespace set. */
    const uint32_t ws[] = {
        0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x20,
        0x00A0, 0x1680,
        0x2000, 0x2001, 0x2002, 0x2003, 0x2004, 0x2005,
        0x2006, 0x2007, 0x2008, 0x2009, 0x200A,
        0x202F, 0x205F, 0x3000,
    };
    for (size_t i = 0; i < sizeof(ws) / sizeof(ws[0]); ++i) {
        MDIT_ASSERT_TRUE(mdit_is_whitespace(ws[i]));
    }
    /* Codepoints that are *Zl* / *Zp* are NOT whitespace per
     * markdown-it. Verify a couple. */
    MDIT_ASSERT_FALSE(mdit_is_whitespace(0x2028)); /* LINE SEPARATOR */
    MDIT_ASSERT_FALSE(mdit_is_whitespace(0x2029)); /* PARAGRAPH SEPARATOR */
    /* Adjacent non-whitespace. */
    MDIT_ASSERT_FALSE(mdit_is_whitespace(0x08));
    MDIT_ASSERT_FALSE(mdit_is_whitespace(0x0E));
    MDIT_ASSERT_FALSE(mdit_is_whitespace(0x21));
    MDIT_ASSERT_FALSE(mdit_is_whitespace(0x9F));
    MDIT_ASSERT_FALSE(mdit_is_whitespace(0xA1));
    /* Out of range. */
    MDIT_ASSERT_FALSE(mdit_is_whitespace(0x110000));
    MDIT_ASSERT_FALSE(mdit_is_whitespace(0xFFFFFFFF));
}

MDIT_TEST(punct_known_codepoints)
{
    /* ASCII punctuation. */
    MDIT_ASSERT_TRUE (mdit_is_punct('!'));
    MDIT_ASSERT_TRUE (mdit_is_punct('('));
    MDIT_ASSERT_TRUE (mdit_is_punct(','));
    MDIT_ASSERT_TRUE (mdit_is_punct('.'));
    MDIT_ASSERT_TRUE (mdit_is_punct(';'));
    MDIT_ASSERT_TRUE (mdit_is_punct('?'));
    MDIT_ASSERT_TRUE (mdit_is_punct('@'));
    MDIT_ASSERT_TRUE (mdit_is_punct('['));
    MDIT_ASSERT_TRUE (mdit_is_punct(']'));
    MDIT_ASSERT_TRUE (mdit_is_punct('{'));
    MDIT_ASSERT_TRUE (mdit_is_punct('}'));
    MDIT_ASSERT_TRUE (mdit_is_punct('~'));
    /* Letters / digits / control chars / spaces are not punct. */
    MDIT_ASSERT_FALSE(mdit_is_punct('A'));
    MDIT_ASSERT_FALSE(mdit_is_punct('z'));
    MDIT_ASSERT_FALSE(mdit_is_punct('0'));
    MDIT_ASSERT_FALSE(mdit_is_punct('9'));
    MDIT_ASSERT_FALSE(mdit_is_punct('\t'));
    MDIT_ASSERT_FALSE(mdit_is_punct(' '));
    MDIT_ASSERT_FALSE(mdit_is_punct(0x00));
    /* Currency / symbols. */
    MDIT_ASSERT_TRUE (mdit_is_punct(0x20AC)); /* € (Sc) */
    MDIT_ASSERT_TRUE (mdit_is_punct(0x00A3)); /* £ (Sc) */
    MDIT_ASSERT_TRUE (mdit_is_punct(0x00AB)); /* « (Pi) */
    MDIT_ASSERT_TRUE (mdit_is_punct(0x00BB)); /* » (Pf) */
    MDIT_ASSERT_TRUE (mdit_is_punct(0x2014)); /* — (Pd) */
    MDIT_ASSERT_TRUE (mdit_is_punct(0x2603)); /* ☃ (So) */
    MDIT_ASSERT_TRUE (mdit_is_punct(0x2200)); /* ∀ (Sm) */
    /* Number-like that are NOT punct. */
    MDIT_ASSERT_FALSE(mdit_is_punct(0x00B9)); /* ¹ (No) */
    /* Out of range. */
    MDIT_ASSERT_FALSE(mdit_is_punct(0x110000));
    MDIT_ASSERT_FALSE(mdit_is_punct(0xFFFFFFFFu));
}

MDIT_TEST(punct_matches_python_unicodedata_for_sample)
{
    size_t n = k_utf_vectors_len;
    MDIT_ASSERT(n > 1000); /* sanity: we expect thousands of samples */
    for (size_t i = 0; i < n; ++i) {
        const mdit_utf_vector *v = &k_utf_vectors[i];
        bool got = mdit_is_punct(v->cp);
        if (got != v->is_punct) {
            char buf[128];
            (void)snprintf(buf, sizeof buf,
                "is_punct(U+%06X): C=%d, Python=%d",
                (unsigned)v->cp, (int)got, (int)v->is_punct);
            mdit_test_fail(__FILE__, __LINE__, buf);
        }
    }
}

MDIT_TEST(whitespace_matches_python_for_sample)
{
    size_t n = k_utf_vectors_len;
    for (size_t i = 0; i < n; ++i) {
        const mdit_utf_vector *v = &k_utf_vectors[i];
        bool got = mdit_is_whitespace(v->cp);
        if (got != v->is_whitespace) {
            char buf[128];
            (void)snprintf(buf, sizeof buf,
                "is_whitespace(U+%06X): C=%d, Python=%d",
                (unsigned)v->cp, (int)got, (int)v->is_whitespace);
            mdit_test_fail(__FILE__, __LINE__, buf);
        }
    }
}

#define MDIT_TEST_REGISTRY                                                 \
    MDIT_TEST_LIST_ENTRY(whitespace_explicit_set),                         \
    MDIT_TEST_LIST_ENTRY(punct_known_codepoints),                          \
    MDIT_TEST_LIST_ENTRY(punct_matches_python_unicodedata_for_sample),     \
    MDIT_TEST_LIST_ENTRY(whitespace_matches_python_for_sample)

#include "mdit_test_main.h"
