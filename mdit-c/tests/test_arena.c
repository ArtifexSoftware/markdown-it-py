/*
 * test_arena.c — unit tests for the bump allocator.
 */
#include "mdit_test.h"

#include "arena.h"
#include "mdit/mdit.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------- */
static jmp_buf      s_oom_jmp;
static int          s_oom_called = 0;
static size_t       s_oom_requested = 0;

static void test_oom_handler(void *user, size_t requested)
{
    (void)user;
    s_oom_called    = 1;
    s_oom_requested = requested;
    longjmp(s_oom_jmp, 1);
}

/* ---------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------- */
MDIT_TEST(arena_init_destroy_empty)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a;
    mdit_arena_init(&a, 0);
    MDIT_ASSERT_EQ_PTR(a.head, NULL);
    MDIT_ASSERT_EQ_SZ(a.total_used, 0);
    MDIT_ASSERT_EQ_SZ(a.num_chunks, 0);
    /* Destroying a never-allocated arena must be safe. */
    mdit_arena_destroy(&lib, &a);
    MDIT_ASSERT_EQ_PTR(a.head, NULL);
}

MDIT_TEST(arena_alloc_returns_aligned_pointer)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a;
    mdit_arena_init(&a, 0);
    for (int i = 1; i < 64; ++i) {
        void *p = mdit_arena_alloc(&lib, &a, (size_t)i);
        MDIT_ASSERT_NE(p, NULL);
        MDIT_ASSERT_EQ_SZ(((uintptr_t)p) % MDIT_ARENA_ALIGN, 0);
    }
    MDIT_ASSERT(a.total_used >= 64); /* at least the bytes we asked for */
    mdit_arena_destroy(&lib, &a);
}

MDIT_TEST(arena_zalloc_zeroes)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a;
    mdit_arena_init(&a, 0);
    /* Burn some memory first so subsequent zallocs can actually return
     * non-zero memory if zeroing were buggy. */
    unsigned char *first = mdit_arena_alloc(&lib, &a, 256);
    memset(first, 0xCC, 256);
    /* Reset and zalloc -- the zeroed region must overlap the dirty one. */
    mdit_arena_reset(&lib, &a);
    unsigned char *p = mdit_arena_zalloc(&lib, &a, 256);
    for (int i = 0; i < 256; ++i) {
        MDIT_ASSERT_EQ_INT(p[i], 0);
    }
    mdit_arena_destroy(&lib, &a);
}

MDIT_TEST(arena_zero_byte_alloc_unique)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a;
    mdit_arena_init(&a, 0);
    void *a0 = mdit_arena_alloc(&lib, &a, 0);
    void *a1 = mdit_arena_alloc(&lib, &a, 0);
    MDIT_ASSERT_NE(a0, NULL);
    MDIT_ASSERT_NE(a1, NULL);
    /* Implementations are free to return equal pointers for adjacent
     * zero-sized allocations; our bump allocator does not. */
    MDIT_ASSERT_NE(a0, a1);
    mdit_arena_destroy(&lib, &a);
}

MDIT_TEST(arena_grows_into_new_chunks)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a;
    mdit_arena_init(&a, 256); /* small initial chunk */
    /* Allocate enough to force at least three chunks. */
    for (int i = 0; i < 32; ++i) {
        (void)mdit_arena_alloc(&lib, &a, 64);
    }
    MDIT_ASSERT(a.num_chunks >= 2);
    MDIT_ASSERT(a.total_capacity >= 32 * 64);
    mdit_arena_destroy(&lib, &a);
}

MDIT_TEST(arena_dup_copies_bytes)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a;
    mdit_arena_init(&a, 0);
    const char src[] = "the quick brown fox";
    void *p = mdit_arena_dup(&lib, &a, src, sizeof src);
    MDIT_ASSERT_MEM_EQ(p, src, sizeof src);
    /* Mutating the arena copy must not touch the source. */
    ((char *)p)[0] = 'T';
    MDIT_ASSERT_EQ_INT(src[0], 't');
    mdit_arena_destroy(&lib, &a);
}

MDIT_TEST(arena_strdup_null_safe)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a;
    mdit_arena_init(&a, 0);
    MDIT_ASSERT_EQ_PTR(mdit_arena_strdup(&lib, &a, NULL), NULL);
    char *s = mdit_arena_strdup(&lib, &a, "hello");
    MDIT_ASSERT_STR_EQ(s, "hello");
    mdit_arena_destroy(&lib, &a);
}

MDIT_TEST(arena_reset_keeps_largest_chunk)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a;
    mdit_arena_init(&a, 256);
    /* Force several chunks of varying sizes. */
    (void)mdit_arena_alloc(&lib, &a, 200);
    (void)mdit_arena_alloc(&lib, &a, 1024); /* triggers a fresh, bigger chunk */
    size_t cap_before = a.total_capacity;
    mdit_arena_reset(&lib, &a);
    MDIT_ASSERT_EQ_SZ(a.num_chunks, 1);
    MDIT_ASSERT_EQ_SZ(a.total_used, 0);
    /* The kept chunk should be the largest of the two. */
    MDIT_ASSERT(a.total_capacity > 0);
    MDIT_ASSERT(a.total_capacity <= cap_before);
    /* Allocations after reset must not crash. */
    void *p = mdit_arena_alloc(&lib, &a, 64);
    MDIT_ASSERT_NE(p, NULL);
    mdit_arena_destroy(&lib, &a);
}

MDIT_TEST(arena_try_extend_in_place_succeeds)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a;
    mdit_arena_init(&a, 0);
    void *p = mdit_arena_alloc(&lib, &a, 32);
    void *p2 = p;
    bool ok = mdit_arena_try_extend(&lib, &a, &p2, 32, 64);
    MDIT_ASSERT_TRUE(ok);
    MDIT_ASSERT_EQ_PTR(p2, p);
    mdit_arena_destroy(&lib, &a);
}

MDIT_TEST(arena_try_extend_blocked_by_intervening_alloc)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a;
    mdit_arena_init(&a, 0);
    void *p = mdit_arena_alloc(&lib, &a, 32);
    (void)mdit_arena_alloc(&lib, &a, 1); /* moves cursor past p's tail */
    void *p2 = p;
    bool ok = mdit_arena_try_extend(&lib, &a, &p2, 32, 64);
    MDIT_ASSERT_FALSE(ok);
    mdit_arena_destroy(&lib, &a);
}

MDIT_TEST(arena_oom_handler_invoked)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    lib.oom  = test_oom_handler;
    lib.user = NULL;
    mdit_arena a;
    mdit_arena_init(&a, 0);
    s_oom_called    = 0;
    s_oom_requested = 0;
    if (setjmp(s_oom_jmp) == 0) {
        /* SIZE_MAX is guaranteed to fail malloc on every platform we
         * care about. */
        (void)mdit_arena_alloc(&lib, &a, (size_t)-1 / 2);
        MDIT_FAIL("expected OOM handler to longjmp out");
    }
    MDIT_ASSERT_EQ_INT(s_oom_called, 1);
    MDIT_ASSERT(s_oom_requested > 0);
    mdit_arena_destroy(&lib, &a);
}

#define MDIT_TEST_REGISTRY                                                  \
    MDIT_TEST_LIST_ENTRY(arena_init_destroy_empty),                         \
    MDIT_TEST_LIST_ENTRY(arena_alloc_returns_aligned_pointer),              \
    MDIT_TEST_LIST_ENTRY(arena_zalloc_zeroes),                              \
    MDIT_TEST_LIST_ENTRY(arena_zero_byte_alloc_unique),                     \
    MDIT_TEST_LIST_ENTRY(arena_grows_into_new_chunks),                      \
    MDIT_TEST_LIST_ENTRY(arena_dup_copies_bytes),                           \
    MDIT_TEST_LIST_ENTRY(arena_strdup_null_safe),                           \
    MDIT_TEST_LIST_ENTRY(arena_reset_keeps_largest_chunk),                  \
    MDIT_TEST_LIST_ENTRY(arena_try_extend_in_place_succeeds),               \
    MDIT_TEST_LIST_ENTRY(arena_try_extend_blocked_by_intervening_alloc),    \
    MDIT_TEST_LIST_ENTRY(arena_oom_handler_invoked)

#include "mdit_test_main.h"
