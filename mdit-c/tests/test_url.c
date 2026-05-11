/*
 * test_url.c — mdurl port (parse + format + encode + decode).
 *
 * Each case in url_vectors.h carries the original input plus the
 * exact bytes Python's mdurl would emit. The C tests run the port and
 * assert byte equality against those references.
 */
#include "mdit_test.h"

#include "arena.h"
#include "mdit/mdit.h"
#include "json.h"
#include "str.h"
#include "url.h"

#include "url_vectors.h"

#include <string.h>

/* ---------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------- */
static void check_field(const char *label,
                        const char *case_input,
                        bool expected_present, const char *expected,
                        size_t expected_len,
                        bool actual_present, mdit_str actual)
{
    if (expected_present != actual_present) {
        char msg[512];
        (void)snprintf(msg, sizeof msg,
            "[%s] presence mismatch for input %s: expected=%d actual=%d",
            label, case_input,
            (int)expected_present, (int)actual_present);
        mdit_test_fail(__FILE__, __LINE__, msg);
        return;
    }
    if (!expected_present) return;
    if (actual.len != expected_len ||
        memcmp(actual.data, expected, actual.len) != 0) {
        char msg[1024];
        (void)snprintf(msg, sizeof msg,
            "[%s] value mismatch for input %s\n  got:  %.*s (len %zu)\n  want: %.*s (len %zu)",
            label, case_input,
            (int)actual.len, actual.data, actual.len,
            (int)expected_len, expected, expected_len);
        mdit_test_fail(__FILE__, __LINE__, msg);
    }
}

/* ---------------------------------------------------------------------
 * Parse + format
 * ------------------------------------------------------------------- */
MDIT_TEST(url_parse_matches_python_for_sample)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    for (size_t i = 0; i < k_url_parse_cases_len; ++i) {
        const mdit_url_parse_case *c = &k_url_parse_cases[i];
        mdit_arena a; mdit_arena_init(&a, 0);
        mdit_url u;
        mdit_str input;
        input.data = c->input; input.len = c->input_len;
        if (!mdit_url_parse(&lib, &a, input, false, &u)) {
            mdit_test_fail(__FILE__, __LINE__, "mdit_url_parse: OOM");
        }

        check_field("protocol", c->input,
                    c->protocol.present, c->protocol.data, c->protocol.len,
                    u.has_protocol, u.protocol);
        check_field("auth", c->input,
                    c->auth.present, c->auth.data, c->auth.len,
                    u.has_auth, u.auth);
        check_field("port", c->input,
                    c->port.present, c->port.data, c->port.len,
                    u.has_port, u.port);
        check_field("hostname", c->input,
                    c->hostname.present, c->hostname.data, c->hostname.len,
                    u.has_hostname, u.hostname);
        check_field("search", c->input,
                    c->search.present, c->search.data, c->search.len,
                    u.has_search, u.search);
        check_field("hash", c->input,
                    c->hash.present, c->hash.data, c->hash.len,
                    u.has_hash, u.hash);
        check_field("pathname", c->input,
                    c->pathname.present, c->pathname.data, c->pathname.len,
                    u.has_pathname, u.pathname);

        if (u.slashes != c->slashes) {
            char msg[256];
            (void)snprintf(msg, sizeof msg,
                "slashes mismatch for input %s: got %d want %d",
                c->input, (int)u.slashes, (int)c->slashes);
            mdit_test_fail(__FILE__, __LINE__, msg);
        }

        mdit_arena_destroy(&lib, &a);
    }
}

MDIT_TEST(url_format_matches_python_for_sample)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    for (size_t i = 0; i < k_url_parse_cases_len; ++i) {
        const mdit_url_parse_case *c = &k_url_parse_cases[i];
        mdit_arena a; mdit_arena_init(&a, 0);
        mdit_url u;
        mdit_str input;
        input.data = c->input; input.len = c->input_len;
        (void)mdit_url_parse(&lib, &a, input, false, &u);

        mdit_buf b; mdit_buf_init(&b);
        if (!mdit_url_format(&u, &b)) {
            mdit_buf_destroy(&b);
            mdit_arena_destroy(&lib, &a);
            mdit_test_fail(__FILE__, __LINE__, "mdit_url_format: OOM");
        }
        if (mdit_buf_len(&b) != c->format_len ||
            memcmp(mdit_buf_str(&b), c->format, c->format_len) != 0) {
            char msg[1024];
            (void)snprintf(msg, sizeof msg,
                "format mismatch for input %s\n  got:  %s\n  want: %s",
                c->input, mdit_buf_str(&b), c->format);
            mdit_buf_destroy(&b);
            mdit_arena_destroy(&lib, &a);
            mdit_test_fail(__FILE__, __LINE__, msg);
        }
        mdit_buf_destroy(&b);
        mdit_arena_destroy(&lib, &a);
    }
}

/* ---------------------------------------------------------------------
 * Encode
 * ------------------------------------------------------------------- */
static void run_encode_cases(const mdit_url_encode_case *cases,
                             size_t n, const char *exclude,
                             const char *label)
{
    for (size_t i = 0; i < n; ++i) {
        const mdit_url_encode_case *c = &cases[i];
        mdit_buf b; mdit_buf_init(&b);
        mdit_str input;
        input.data = c->input; input.len = c->input_len;
        if (!mdit_url_encode(input, exclude, true, &b)) {
            mdit_buf_destroy(&b);
            mdit_test_fail(__FILE__, __LINE__, "encode: OOM");
        }
        if (mdit_buf_len(&b) != c->output_len ||
            memcmp(mdit_buf_str(&b), c->output, c->output_len) != 0) {
            char msg[1024];
            (void)snprintf(msg, sizeof msg,
                "[%s] encode mismatch\n  in:   %s\n  got:  %s\n  want: %s",
                label, c->input, mdit_buf_str(&b), c->output);
            mdit_buf_destroy(&b);
            mdit_test_fail(__FILE__, __LINE__, msg);
        }
        mdit_buf_destroy(&b);
    }
}

MDIT_TEST(url_encode_default_matches_python)
{
    run_encode_cases(k_url_encode_default_cases,
                     k_url_encode_default_cases_len,
                     MDIT_URL_ENCODE_DEFAULT_CHARS,
                     "encode_default");
}

MDIT_TEST(url_encode_component_matches_python)
{
    run_encode_cases(k_url_encode_component_cases,
                     k_url_encode_component_cases_len,
                     MDIT_URL_ENCODE_COMPONENT_CHARS,
                     "encode_component");
}

/* ---------------------------------------------------------------------
 * Decode
 * ------------------------------------------------------------------- */
static void run_decode_cases(const mdit_url_encode_case *cases,
                             size_t n, const char *exclude,
                             const char *label)
{
    for (size_t i = 0; i < n; ++i) {
        const mdit_url_encode_case *c = &cases[i];
        mdit_buf b; mdit_buf_init(&b);
        mdit_str input;
        input.data = c->input; input.len = c->input_len;
        if (!mdit_url_decode(input, exclude, &b)) {
            mdit_buf_destroy(&b);
            mdit_test_fail(__FILE__, __LINE__, "decode: OOM");
        }
        if (mdit_buf_len(&b) != c->output_len ||
            memcmp(mdit_buf_str(&b), c->output, c->output_len) != 0) {
            char msg[1024];
            (void)snprintf(msg, sizeof msg,
                "[%s] decode mismatch\n  in:   %s\n  got:  %s\n  want: %s",
                label, c->input, mdit_buf_str(&b), c->output);
            mdit_buf_destroy(&b);
            mdit_test_fail(__FILE__, __LINE__, msg);
        }
        mdit_buf_destroy(&b);
    }
}

MDIT_TEST(url_decode_default_matches_python)
{
    run_decode_cases(k_url_decode_default_cases,
                     k_url_decode_default_cases_len,
                     MDIT_URL_DECODE_DEFAULT_CHARS,
                     "decode_default");
}

MDIT_TEST(url_decode_component_matches_python)
{
    run_decode_cases(k_url_decode_component_cases,
                     k_url_decode_component_cases_len,
                     MDIT_URL_DECODE_COMPONENT_CHARS,
                     "decode_component");
}

/* ---------------------------------------------------------------------
 * Direct API smoke tests not driven by vectors.
 * ------------------------------------------------------------------- */
MDIT_TEST(url_format_roundtrip_basic)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_url u;
    if (!mdit_url_parse(&lib, &a, MDIT_STR_LIT("https://e.org/p?q#h"), false, &u)) {
        mdit_arena_destroy(&lib, &a);
        mdit_test_fail(__FILE__, __LINE__, "parse failed");
    }
    MDIT_ASSERT_TRUE(u.has_protocol);
    MDIT_ASSERT_TRUE(mdit_str_eq_z(u.protocol, "https:"));
    MDIT_ASSERT_TRUE(u.slashes);
    MDIT_ASSERT_TRUE(u.has_hostname);
    MDIT_ASSERT_TRUE(mdit_str_eq_z(u.hostname, "e.org"));
    MDIT_ASSERT_TRUE(u.has_pathname);
    MDIT_ASSERT_TRUE(mdit_str_eq_z(u.pathname, "/p"));
    MDIT_ASSERT_TRUE(u.has_search);
    MDIT_ASSERT_TRUE(mdit_str_eq_z(u.search, "?q"));
    MDIT_ASSERT_TRUE(u.has_hash);
    MDIT_ASSERT_TRUE(mdit_str_eq_z(u.hash, "#h"));

    mdit_buf b; mdit_buf_init(&b);
    MDIT_ASSERT_TRUE(mdit_url_format(&u, &b));
    MDIT_ASSERT_STR_EQ(mdit_buf_str(&b), "https://e.org/p?q#h");
    mdit_buf_destroy(&b);

    mdit_arena_destroy(&lib, &a);
}

MDIT_TEST(url_encode_skips_already_escaped_when_keep_escaped)
{
    mdit_buf b; mdit_buf_init(&b);
    MDIT_ASSERT_TRUE(mdit_url_encode(MDIT_STR_LIT("a%20b"),
        MDIT_URL_ENCODE_DEFAULT_CHARS, true, &b));
    MDIT_ASSERT_STR_EQ(mdit_buf_str(&b), "a%20b");
    mdit_buf_destroy(&b);

    /* keep_escaped=false should re-encode the % sign as %25. */
    mdit_buf c; mdit_buf_init(&c);
    MDIT_ASSERT_TRUE(mdit_url_encode(MDIT_STR_LIT("a%20b"),
        MDIT_URL_ENCODE_DEFAULT_CHARS, false, &c));
    MDIT_ASSERT_STR_EQ(mdit_buf_str(&c), "a%2520b");
    mdit_buf_destroy(&c);
}

#define MDIT_TEST_REGISTRY                                                 \
    MDIT_TEST_LIST_ENTRY(url_parse_matches_python_for_sample),             \
    MDIT_TEST_LIST_ENTRY(url_format_matches_python_for_sample),            \
    MDIT_TEST_LIST_ENTRY(url_encode_default_matches_python),               \
    MDIT_TEST_LIST_ENTRY(url_encode_component_matches_python),             \
    MDIT_TEST_LIST_ENTRY(url_decode_default_matches_python),               \
    MDIT_TEST_LIST_ENTRY(url_decode_component_matches_python),             \
    MDIT_TEST_LIST_ENTRY(url_format_roundtrip_basic),                      \
    MDIT_TEST_LIST_ENTRY(url_encode_skips_already_escaped_when_keep_escaped)

#include "mdit_test_main.h"
