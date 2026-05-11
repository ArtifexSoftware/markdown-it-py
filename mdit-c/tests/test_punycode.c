/*
 * test_punycode.c — RFC 3492 codec + IDN domain wrappers.
 *
 * The expected outputs below are pinned to Python's ``codecs.encode``,
 * ``codecs.decode``, ``markdown_it._punycode.to_ascii``, and
 * ``markdown_it._punycode.to_unicode``. Any drift here would surface
 * as a token-oracle failure on the linkify rows; this suite gives us
 * faster feedback on the codec layer in isolation.
 */
#include "mdit_test.h"

#include <string.h>

#include "arena.h"
#include "mdit/mdit.h"
#include "json.h"
#include "punycode.h"
#include "str.h"

/* ---------------------------------------------------------------------
 * Codec helpers
 * ------------------------------------------------------------------- */
static void check_encode(const uint32_t *cps, size_t n,
                         const char *want, size_t want_len)
{
    mdit_buf b; mdit_buf_init_default(&b);
    if (!mdit_punycode_encode_cps(cps, n, &b)) {
        mdit_buf_destroy(&b);
        mdit_test_fail(__FILE__, __LINE__, "punycode_encode: returned false");
        return;
    }
    if (b.len != want_len || memcmp(b.data ? b.data : "", want, want_len) != 0) {
        char msg[256];
        (void)snprintf(msg, sizeof msg,
            "encode mismatch:\n  got:  %.*s\n  want: %.*s",
            (int)b.len, b.data ? b.data : "",
            (int)want_len, want);
        mdit_buf_destroy(&b);
        mdit_test_fail(__FILE__, __LINE__, msg);
        return;
    }
    mdit_buf_destroy(&b);
}

static void check_decode_ok(const char *in, size_t in_len,
                            const char *want_utf8, size_t want_len)
{
    mdit_str input = { in, in_len };
    mdit_buf b; mdit_buf_init_default(&b);
    if (!mdit_punycode_decode_to_utf8(input, &b)) {
        mdit_buf_destroy(&b);
        mdit_test_fail(__FILE__, __LINE__, "punycode_decode: returned false");
        return;
    }
    if (b.len != want_len || memcmp(b.data ? b.data : "", want_utf8, want_len) != 0) {
        char msg[256];
        (void)snprintf(msg, sizeof msg,
            "decode mismatch for %.*s:\n  got:  %.*s\n  want: %.*s",
            (int)in_len, in,
            (int)b.len, b.data ? b.data : "",
            (int)want_len, want_utf8);
        mdit_buf_destroy(&b);
        mdit_test_fail(__FILE__, __LINE__, msg);
        return;
    }
    mdit_buf_destroy(&b);
}

static void check_decode_fail(const char *in, size_t in_len)
{
    mdit_str input = { in, in_len };
    mdit_buf b; mdit_buf_init_default(&b);
    if (mdit_punycode_decode_to_utf8(input, &b)) {
        char msg[128];
        (void)snprintf(msg, sizeof msg,
            "decode unexpectedly succeeded for %.*s", (int)in_len, in);
        mdit_buf_destroy(&b);
        mdit_test_fail(__FILE__, __LINE__, msg);
        return;
    }
    mdit_buf_destroy(&b);
}

/* ---------------------------------------------------------------------
 * Codec: known-good vectors from CPython codecs.encode/decode.
 * ------------------------------------------------------------------- */
MDIT_TEST(punycode_encode_known_vectors)
{
    /* "рф" (U+0440 U+0444) -> "p1ai". No basic codepoints, no
     * trailing '-'. */
    {
        const uint32_t cps[] = { 0x0440, 0x0444 };
        check_encode(cps, 2, "p1ai", 4);
    }
    /* "президент" (Cyrillic) -> "d1abbgf6aiiy". */
    {
        const uint32_t cps[] = {
            0x043F, 0x0440, 0x0435, 0x0437, 0x0438, 0x0434, 0x0435, 0x043D, 0x0442
        };
        check_encode(cps, 9, "d1abbgf6aiiy", 12);
    }
    /* All-ASCII input still gets a trailing '-' delimiter. */
    {
        const uint32_t cps[] = { 'a', 'b', 'c' };
        check_encode(cps, 3, "abc-", 4);
    }
    /* Mixed ASCII with hyphen. */
    {
        const uint32_t cps[] = { 'a', '-', 'b' };
        check_encode(cps, 3, "a-b-", 4);
    }
    /* Empty input -> empty output. */
    {
        check_encode(NULL, 0, "", 0);
    }
}

MDIT_TEST(punycode_decode_known_vectors)
{
    /* "p1ai" -> "рф" (UTF-8 bytes D1 80 D1 84). */
    check_decode_ok("p1ai", 4, "\xD1\x80\xD1\x84", 4);

    /* "d1abbgf6aiiy" -> "президент". */
    check_decode_ok("d1abbgf6aiiy", 12,
                    /* UTF-8 of президент */
                    "\xD0\xBF\xD1\x80\xD0\xB5\xD0\xB7\xD0\xB8\xD0\xB4\xD0\xB5\xD0\xBD\xD1\x82",
                    18);

    /* "abc-" -> "abc". */
    check_decode_ok("abc-", 4, "abc", 3);

    /* "a-b-" -> "a-b". */
    check_decode_ok("a-b-", 4, "a-b", 3);

    /* Empty -> empty. */
    check_decode_ok("", 0, "", 0);

    /* Round-trip an all-ASCII no-trailer input also works (the codec
     * treats no-hyphen as no basic codepoints, then decodes the whole
     * thing as the extended sequence). The expected output here is the
     * same as Python's ``codecs.decode`` produces. */
    /* "p1ai" already covered above as the canonical no-basic case. */
}

MDIT_TEST(punycode_decode_rejects_malformed)
{
    /* Invalid digit. */
    check_decode_fail("p1$ai", 5);

    /* Truncated extended part — the loop demands an extra digit when
     * `digit >= t`, so a single 'z' which is digit=25 (>= TMAX=26 is
     * false; t starts at TMIN=1; w expansion happens only if digit>=t).
     * A single digit 'a' (=0) terminates the loop immediately, so we
     * pick a sequence that starts the loop and never closes it. */
    check_decode_fail("zzzzzzzzzzzzzzzzzzzzzzzzz", 25);
}

/* ---------------------------------------------------------------------
 * Domain wrappers: ``mdit_idn_to_ascii`` / ``mdit_idn_to_unicode``.
 * ------------------------------------------------------------------- */
static void check_idn_ascii(const char *in, size_t in_len,
                            const char *want, size_t want_len)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_str input = { in, in_len };
    mdit_str out;
    if (!mdit_idn_to_ascii(&lib, &a, input, &out)) {
        mdit_arena_destroy(&lib, &a);
        mdit_test_fail(__FILE__, __LINE__, "idn_to_ascii: false");
        return;
    }
    if (out.len != want_len || memcmp(out.data, want, want_len) != 0) {
        char msg[512];
        (void)snprintf(msg, sizeof msg,
            "idn_to_ascii mismatch for %.*s:\n  got:  %.*s\n  want: %.*s",
            (int)in_len, in,
            (int)out.len, out.data,
            (int)want_len, want);
        mdit_arena_destroy(&lib, &a);
        mdit_test_fail(__FILE__, __LINE__, msg);
        return;
    }
    mdit_arena_destroy(&lib, &a);
}

static void check_idn_unicode(const char *in, size_t in_len,
                              const char *want, size_t want_len)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_str input = { in, in_len };
    mdit_str out;
    if (!mdit_idn_to_unicode(&lib, &a, input, &out)) {
        mdit_arena_destroy(&lib, &a);
        mdit_test_fail(__FILE__, __LINE__, "idn_to_unicode: false");
        return;
    }
    if (out.len != want_len || memcmp(out.data, want, want_len) != 0) {
        char msg[512];
        (void)snprintf(msg, sizeof msg,
            "idn_to_unicode mismatch for %.*s:\n  got:  %.*s\n  want: %.*s",
            (int)in_len, in,
            (int)out.len, out.data,
            (int)want_len, want);
        mdit_arena_destroy(&lib, &a);
        mdit_test_fail(__FILE__, __LINE__, msg);
        return;
    }
    mdit_arena_destroy(&lib, &a);
}

#define LIT(x) (x), (sizeof(x) - 1)

MDIT_TEST(idn_to_ascii_matches_python)
{
    /* "президент.рф" -> "xn--d1abbgf6aiiy.xn--p1ai". */
    check_idn_ascii(LIT("\xD0\xBF\xD1\x80\xD0\xB5\xD0\xB7\xD0\xB8\xD0\xB4\xD0\xB5\xD0\xBD\xD1\x82.\xD1\x80\xD1\x84"),
                    LIT("xn--d1abbgf6aiiy.xn--p1ai"));

    /* All-ASCII passes through unchanged (no per-label encoding, no
     * trailing '-'). */
    check_idn_ascii(LIT("example.com"), LIT("example.com"));

    /* Empty domain -> empty. */
    check_idn_ascii(LIT(""), LIT(""));

    /* Email split: only the part after '@' is encoded. */
    check_idn_ascii(LIT("user@\xD0\xBF\xD1\x80\xD0\xB5\xD0\xB7\xD0\xB8\xD0\xB4\xD0\xB5\xD0\xBD\xD1\x82.\xD1\x80\xD1\x84"),
                    LIT("user@xn--d1abbgf6aiiy.xn--p1ai"));

    /* Subdomain mix: only the non-ASCII label gets the xn-- prefix. */
    check_idn_ascii(LIT("sub.\xD0\xB4\xD0\xBE\xD0\xBC\xD0\xB5\xD0\xBD.org"),
                    LIT("sub.xn--d1acufc.org"));

    /* Alternate dots U+3002 / U+FF0E / U+FF61 are recognized as label
     * separators and normalized to U+002E. */
    check_idn_ascii(LIT("\xE3\x80\x82"),       /* lone U+3002 */
                    LIT("."));
    check_idn_ascii(LIT("a\xE3\x80\x82\xD1\x80\xD1\x84"),
                    LIT("a.xn--p1ai"));
}

MDIT_TEST(idn_to_unicode_matches_python)
{
    /* Round-trip from the encode tests. */
    check_idn_unicode(LIT("xn--d1abbgf6aiiy.xn--p1ai"),
                      LIT("\xD0\xBF\xD1\x80\xD0\xB5\xD0\xB7\xD0\xB8\xD0\xB4\xD0\xB5\xD0\xBD\xD1\x82.\xD1\x80\xD1\x84"));

    /* No xn-- prefix: pass through. */
    check_idn_unicode(LIT("example.com"), LIT("example.com"));

    /* Python's startswith("xn--") is strictly case-sensitive — an
     * uppercase ``XN--P1AI`` is *not* decoded. */
    check_idn_unicode(LIT("EXAMPLE.XN--P1AI"), LIT("EXAMPLE.XN--P1AI"));

    /* Mixed: only labels with an exact lowercase xn-- prefix get
     * decoded. The trailing punycode body is case-tolerant so an
     * upper-cased body still decodes (the wrapper lowercases it
     * before the codec call). */
    check_idn_unicode(LIT("a.xn--P1AI"), LIT("a.\xD1\x80\xD1\x84"));

    /* Empty string. */
    check_idn_unicode(LIT(""), LIT(""));
}

#define MDIT_TEST_REGISTRY                                              \
    MDIT_TEST_LIST_ENTRY(punycode_encode_known_vectors),                \
    MDIT_TEST_LIST_ENTRY(punycode_decode_known_vectors),                \
    MDIT_TEST_LIST_ENTRY(punycode_decode_rejects_malformed),            \
    MDIT_TEST_LIST_ENTRY(idn_to_ascii_matches_python),                  \
    MDIT_TEST_LIST_ENTRY(idn_to_unicode_matches_python)

#include "mdit_test_main.h"
