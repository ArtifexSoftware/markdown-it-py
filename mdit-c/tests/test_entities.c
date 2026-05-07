/*
 * test_entities.c — HTML5 entity-table lookup tests.
 *
 * Coverage:
 *   - Specific high-value hits (amp, lt, gt, quot, ...) including
 *     case-distinct aliases (AMP vs amp).
 *   - Multi-codepoint values (acE, bnequiv).
 *   - Supplementary-plane values (Zscr, zopf).
 *   - Near-miss inputs that must NOT resolve.
 *   - Cross-check against an auto-generated header populated from
 *     html.entities.html5 (keeps C and Python in lockstep).
 */
#include "mdit_test.h"

#include "entities.h"

#include "entity_vectors.h"

#include <string.h>

MDIT_TEST(entities_count_matches_python)
{
    /* html.entities.html5 distilled by mdit-c/scripts/gen_entities.py
     * (semicolon-stripped, deduplicated) currently exposes 2125
     * entries. If Python's table changes, regenerate. */
    MDIT_ASSERT_EQ_SZ(mdit_entity_count(), 2125);
}

MDIT_TEST(entities_amp_lt_gt)
{
    const char *out = NULL;
    size_t      out_len = 0;
    MDIT_ASSERT_TRUE(mdit_entity_lookup("amp", 3, &out, &out_len));
    MDIT_ASSERT_EQ_SZ(out_len, 1);
    MDIT_ASSERT_EQ_INT((unsigned char)out[0], '&');

    MDIT_ASSERT_TRUE(mdit_entity_lookup("lt", 2, &out, &out_len));
    MDIT_ASSERT_EQ_SZ(out_len, 1);
    MDIT_ASSERT_EQ_INT((unsigned char)out[0], '<');

    MDIT_ASSERT_TRUE(mdit_entity_lookup("gt", 2, &out, &out_len));
    MDIT_ASSERT_EQ_SZ(out_len, 1);
    MDIT_ASSERT_EQ_INT((unsigned char)out[0], '>');
}

MDIT_TEST(entities_case_sensitive)
{
    const char *out = NULL;
    size_t      out_len = 0;
    /* Both 'AMP' and 'amp' exist as keys. */
    MDIT_ASSERT_TRUE(mdit_entity_lookup("AMP", 3, &out, &out_len));
    MDIT_ASSERT_EQ_SZ(out_len, 1);
    MDIT_ASSERT_EQ_INT((unsigned char)out[0], '&');
    /* 'amP' (mixed) does NOT exist. */
    MDIT_ASSERT_FALSE(mdit_entity_lookup("amP", 3, &out, &out_len));
    MDIT_ASSERT_EQ_PTR(out, NULL);
    MDIT_ASSERT_EQ_SZ(out_len, 0);
}

MDIT_TEST(entities_unknown_misses)
{
    const char *out = NULL;
    size_t      out_len = 99;
    MDIT_ASSERT_FALSE(mdit_entity_lookup("notarealentity", 14, &out, &out_len));
    MDIT_ASSERT_EQ_PTR(out, NULL);
    MDIT_ASSERT_EQ_SZ(out_len, 0);
}

MDIT_TEST(entities_empty_and_null)
{
    /* Pre-seed the out parameters with non-default sentinels so we can
     * verify the lookup overwrites them on a miss. We use uintptr_t
     * so the cast is portable on both LP64 and ILP32. */
    const char *out = (const char *)(uintptr_t)0x1u;
    size_t      out_len = 99;
    MDIT_ASSERT_FALSE(mdit_entity_lookup(NULL, 5, &out, &out_len));
    MDIT_ASSERT_EQ_PTR(out, NULL);
    MDIT_ASSERT_EQ_SZ(out_len, 0);

    MDIT_ASSERT_FALSE(mdit_entity_lookup("anything", 0, &out, &out_len));
}

MDIT_TEST(entities_multi_codepoint_value)
{
    /* &acE; expands to U+223E (∾) followed by U+0333 (combining double low line). */
    const char *out = NULL;
    size_t      out_len = 0;
    MDIT_ASSERT_TRUE(mdit_entity_lookup("acE", 3, &out, &out_len));
    MDIT_ASSERT_EQ_SZ(out_len, 5);  /* 3 bytes + 2 bytes utf8 */
    /* 0xE2 0x88 0xBE 0xCC 0xB3 */
    const unsigned char want[] = { 0xE2, 0x88, 0xBE, 0xCC, 0xB3 };
    for (size_t i = 0; i < sizeof want; ++i) {
        MDIT_ASSERT_EQ_INT((unsigned char)out[i], want[i]);
    }
}

MDIT_TEST(entities_supplementary_plane)
{
    /* &zopf; -> U+1D56B (𝕫) */
    const char *out = NULL;
    size_t      out_len = 0;
    MDIT_ASSERT_TRUE(mdit_entity_lookup("zopf", 4, &out, &out_len));
    MDIT_ASSERT_EQ_SZ(out_len, 4);
    const unsigned char want[] = { 0xF0, 0x9D, 0x95, 0xAB };
    for (size_t i = 0; i < sizeof want; ++i) {
        MDIT_ASSERT_EQ_INT((unsigned char)out[i], want[i]);
    }
}

MDIT_TEST(entities_match_python_for_sample)
{
    for (size_t i = 0; i < k_entity_hits_len; ++i) {
        const mdit_entity_hit *h = &k_entity_hits[i];
        const char *out = NULL;
        size_t out_len = 0;
        if (!mdit_entity_lookup(h->name, h->name_len, &out, &out_len)) {
            char buf[256];
            (void)snprintf(buf, sizeof buf,
                "expected hit for entity %.*s", (int)h->name_len, h->name);
            mdit_test_fail(__FILE__, __LINE__, buf);
        }
        if (out_len != h->value_len ||
            memcmp(out, h->value, out_len) != 0) {
            char buf[256];
            (void)snprintf(buf, sizeof buf,
                "value mismatch for entity %.*s (got %zu bytes, want %zu)",
                (int)h->name_len, h->name, out_len, h->value_len);
            mdit_test_fail(__FILE__, __LINE__, buf);
        }
    }
    for (size_t i = 0; i < k_entity_misses_len; ++i) {
        const mdit_entity_miss *m = &k_entity_misses[i];
        if (mdit_entity_lookup(m->name, m->name_len, NULL, NULL)) {
            char buf[256];
            (void)snprintf(buf, sizeof buf,
                "expected miss for %.*s", (int)m->name_len, m->name);
            mdit_test_fail(__FILE__, __LINE__, buf);
        }
    }
}

#define MDIT_TEST_REGISTRY                                                 \
    MDIT_TEST_LIST_ENTRY(entities_count_matches_python),                   \
    MDIT_TEST_LIST_ENTRY(entities_amp_lt_gt),                              \
    MDIT_TEST_LIST_ENTRY(entities_case_sensitive),                         \
    MDIT_TEST_LIST_ENTRY(entities_unknown_misses),                         \
    MDIT_TEST_LIST_ENTRY(entities_empty_and_null),                         \
    MDIT_TEST_LIST_ENTRY(entities_multi_codepoint_value),                  \
    MDIT_TEST_LIST_ENTRY(entities_supplementary_plane),                    \
    MDIT_TEST_LIST_ENTRY(entities_match_python_for_sample)

#include "mdit_test_main.h"
