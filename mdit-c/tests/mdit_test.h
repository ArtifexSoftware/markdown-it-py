/*
 * mdit_test.h — tiny header-only test harness for the C port.
 *
 * Design goals:
 *   - Zero dependencies, C11, MSVC-clean.
 *   - One translation unit per test executable; each TU defines a list
 *     of tests and includes mdit_test_main.h at the very bottom to pick
 *     up a generated main(). That keeps fixtures local and ctest reports
 *     one suite per file (matches how `cmark` lays its tests out).
 *   - Tests are registered via a macro that emits a static descriptor
 *     into a TU-local array; no constructors, no link-time tricks.
 *
 * Usage (see tests/test_arena.c for a worked example):
 *
 *     #include "mdit_test.h"
 *     #include "arena.h"
 *
 *     MDIT_TEST(arena_alloc_basic) {
 *         mdit_arena a;
 *         mdit_arena_init(&a, 0);
 *         char *p = mdit_arena_alloc(&a, 32);
 *         MDIT_ASSERT_NE(p, NULL);
 *         mdit_arena_destroy(&a);
 *     }
 *
 *     #include "mdit_test_main.h"
 *
 * Assertion macros log file:line + the expression that failed and call
 * `mdit_test_fail()` which longjmps back to the runner so the rest of
 * the executable can keep running other tests.
 */
#ifndef MDIT_TEST_H
#define MDIT_TEST_H

#include <inttypes.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------- */
typedef void (*mdit_test_fn)(void);

typedef struct mdit_test_case {
    const char  *name;
    const char  *file;
    mdit_test_fn run;
} mdit_test_case;

/* The current test's failure-jump target; threaded as a TU-local global
 * because each test executable is single-threaded. */
extern jmp_buf      mdit_test_jmp;
extern int          mdit_test_failed_count;
extern const char  *mdit_test_current_name;

/* Defined per TU through the registration macros below. */
extern const mdit_test_case  mdit_test_registry[];
extern const size_t          mdit_test_registry_len;

void mdit_test_fail(const char *file, int line, const char *msg);

/* ---------------------------------------------------------------------
 * Registration
 *
 * MDIT_TEST(name) {
 *     ...body...
 * }
 *
 * expands to a static function plus an entry in a registry that the
 * generated main() iterates over.
 *
 * Implementation note: we can't auto-collect descriptors across multiple
 * static arrays in C without linker tricks, so each TU enumerates its
 * own tests by also calling MDIT_TEST_LIST() at the bottom (or just
 * #include "mdit_test_main.h" which does the right thing).
 * ------------------------------------------------------------------- */
#define MDIT_TEST(name)                                                    \
    static void mdit_test__##name(void);                                   \
    static void mdit_test__##name(void)

/* The MDIT_TEST_LIST macro lets the TU emit its registry without
 * repeating function names twice. Call it once per TU like so:
 *
 *   MDIT_TEST_LIST(arena_alloc_basic, arena_alloc_aligned, arena_reset);
 *
 * mdit_test_main.h reads that registry and wires up main().
 */
#define MDIT_TEST_LIST_ENTRY(name)                                          \
    { #name, __FILE__, mdit_test__##name }

/* Cross-platform pretty file:line. */
#define MDIT_TEST__STR_(x) #x
#define MDIT_TEST__STR(x)  MDIT_TEST__STR_(x)

/* ---------------------------------------------------------------------
 * Assertions
 *
 * Each assertion that fails calls mdit_test_fail() which longjmps back
 * to the runner. We pass a short pre-baked message (the source text of
 * the assertion) rather than building printf strings for every check —
 * keeps the harness tiny and avoids heap traffic in the success path.
 * ------------------------------------------------------------------- */
#define MDIT_FAIL(msg)                                                     \
    do {                                                                   \
        mdit_test_fail(__FILE__, __LINE__, (msg));                         \
    } while (0)

#define MDIT_ASSERT(cond)                                                  \
    do {                                                                   \
        if (!(cond)) {                                                     \
            mdit_test_fail(__FILE__, __LINE__,                             \
                           "MDIT_ASSERT(" #cond ")");                      \
        }                                                                  \
    } while (0)

#define MDIT_ASSERT_TRUE(cond)  MDIT_ASSERT((cond))
#define MDIT_ASSERT_FALSE(cond) MDIT_ASSERT(!(cond))

/*
 * NOTE: assertion macros pass the user expression through "%s" rather
 * than concatenating it into the format string. Otherwise an expression
 * like (p % ALIGN) injects a literal '%' into the format spec, which
 * MSVC happily warns about and interprets as garbage at runtime.
 */
#define MDIT_ASSERT_EQ_INT(actual, expected)                               \
    do {                                                                   \
        long long _a = (long long)(actual);                                \
        long long _e = (long long)(expected);                              \
        if (_a != _e) {                                                    \
            char _buf[512];                                                \
            (void)snprintf(_buf, sizeof _buf,                              \
                "MDIT_ASSERT_EQ_INT(%s, %s): got %lld, want %lld",         \
                #actual, #expected, _a, _e);                               \
            mdit_test_fail(__FILE__, __LINE__, _buf);                      \
        }                                                                  \
    } while (0)

#define MDIT_ASSERT_EQ_SZ(actual, expected)                                \
    do {                                                                   \
        size_t _a = (size_t)(actual);                                      \
        size_t _e = (size_t)(expected);                                    \
        if (_a != _e) {                                                    \
            char _buf[512];                                                \
            (void)snprintf(_buf, sizeof _buf,                              \
                "MDIT_ASSERT_EQ_SZ(%s, %s): got %zu, want %zu",            \
                #actual, #expected, _a, _e);                               \
            mdit_test_fail(__FILE__, __LINE__, _buf);                      \
        }                                                                  \
    } while (0)

#define MDIT_ASSERT_EQ_PTR(actual, expected)                               \
    do {                                                                   \
        const void *_a = (const void *)(actual);                           \
        const void *_e = (const void *)(expected);                         \
        if (_a != _e) {                                                    \
            char _buf[512];                                                \
            (void)snprintf(_buf, sizeof _buf,                              \
                "MDIT_ASSERT_EQ_PTR(%s, %s): got %p, want %p",             \
                #actual, #expected, _a, _e);                               \
            mdit_test_fail(__FILE__, __LINE__, _buf);                      \
        }                                                                  \
    } while (0)

#define MDIT_ASSERT_NE(actual, expected)                                   \
    do {                                                                   \
        if ((actual) == (expected)) {                                      \
            char _buf[512];                                                \
            (void)snprintf(_buf, sizeof _buf,                              \
                "MDIT_ASSERT_NE(%s, %s): both equal",                      \
                #actual, #expected);                                       \
            mdit_test_fail(__FILE__, __LINE__, _buf);                      \
        }                                                                  \
    } while (0)

#define MDIT_ASSERT_STR_EQ(actual, expected)                               \
    do {                                                                   \
        const char *_a = (const char *)(actual);                           \
        const char *_e = (const char *)(expected);                         \
        if (_a == NULL || _e == NULL || strcmp(_a, _e) != 0) {             \
            char _buf[1024];                                               \
            (void)snprintf(_buf, sizeof _buf,                              \
                "MDIT_ASSERT_STR_EQ(%s, %s):\n"                            \
                "  got:  %s\n"                                             \
                "  want: %s",                                              \
                #actual, #expected,                                        \
                _a ? _a : "(null)", _e ? _e : "(null)");                   \
            mdit_test_fail(__FILE__, __LINE__, _buf);                      \
        }                                                                  \
    } while (0)

#define MDIT_ASSERT_MEM_EQ(actual, expected, len)                          \
    do {                                                                   \
        if (memcmp((actual), (expected), (len)) != 0) {                    \
            char _buf[512];                                                \
            (void)snprintf(_buf, sizeof _buf,                              \
                "MDIT_ASSERT_MEM_EQ(%s, %s, %s) differ",                   \
                #actual, #expected, #len);                                 \
            mdit_test_fail(__FILE__, __LINE__, _buf);                      \
        }                                                                  \
    } while (0)

#endif /* MDIT_TEST_H */
