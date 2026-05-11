/*
 * test_vec.c — unit tests for the typed dynamic-array wrappers.
 */
#include "mdit_test.h"

#include "arena.h"
#include "mdit/mdit.h"
#include "vec.h"

#include <stddef.h>
#include <stdint.h>

/* Generate two flavours of vec to exercise both growth backends. */
MDIT_VEC_DECLARE(intv, int)
MDIT_VEC_DEFINE(intv, int)

typedef struct kv { int k; int v; } kv;
MDIT_VEC_DECLARE(kvv, kv)
MDIT_VEC_DEFINE(kvv, kv)

MDIT_TEST(vec_arena_push_grows)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a;
    mdit_arena_init(&a, 0);
    mdit_vec_intv v;
    mdit_vec_intv_init(&v, &lib, &a);

    for (int i = 0; i < 1000; ++i) {
        int *slot = mdit_vec_intv_push(&v, i);
        MDIT_ASSERT_NE(slot, NULL);
    }
    MDIT_ASSERT_EQ_SZ(mdit_vec_intv_len(&v), 1000);
    for (int i = 0; i < 1000; ++i) {
        MDIT_ASSERT_EQ_INT(*mdit_vec_intv_at(&v, (size_t)i), i);
    }
    mdit_vec_intv_destroy(&v);
    mdit_arena_destroy(&lib, &a);
}

MDIT_TEST(vec_arena_extends_in_place_when_possible)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a;
    mdit_arena_init(&a, 4096);
    mdit_vec_intv v;
    mdit_vec_intv_init(&v, &lib, &a);
    /* Reserve initial capacity, then push more — most growth steps
     * should be in-place at the arena tail since nothing else lives
     * after the vector. */
    MDIT_ASSERT_TRUE(mdit_vec_intv_reserve(&v, 8));
    for (int i = 0; i < 64; ++i) {
        (void)mdit_vec_intv_push(&v, i * 2);
    }
    /* Spot-check the last element. */
    MDIT_ASSERT_EQ_INT(*mdit_vec_intv_at(&v, 63), 63 * 2);
    mdit_vec_intv_destroy(&v);
    mdit_arena_destroy(&lib, &a);
}

MDIT_TEST(vec_malloc_push_grows)
{
    mdit_vec_intv v;
    mdit_vec_intv_init(&v, NULL, NULL);
    for (int i = 0; i < 500; ++i) {
        (void)mdit_vec_intv_push(&v, i);
    }
    MDIT_ASSERT_EQ_SZ(mdit_vec_intv_len(&v), 500);
    MDIT_ASSERT_EQ_INT(*mdit_vec_intv_at(&v, 0), 0);
    MDIT_ASSERT_EQ_INT(*mdit_vec_intv_at(&v, 499), 499);
    mdit_vec_intv_destroy(&v);
    /* destroy must be idempotent */
    mdit_vec_intv_destroy(&v);
}

MDIT_TEST(vec_pop_clear_emplace)
{
    mdit_vec_kvv v;
    mdit_vec_kvv_init(&v, NULL, NULL);
    for (int i = 0; i < 5; ++i) {
        kv *slot = mdit_vec_kvv_emplace(&v);
        MDIT_ASSERT_NE(slot, NULL);
        slot->k = i;
        slot->v = i * 10;
    }
    MDIT_ASSERT_EQ_SZ(mdit_vec_kvv_len(&v), 5);
    MDIT_ASSERT_EQ_INT(mdit_vec_kvv_at(&v, 4)->v, 40);
    mdit_vec_kvv_pop(&v);
    MDIT_ASSERT_EQ_SZ(mdit_vec_kvv_len(&v), 4);
    mdit_vec_kvv_clear(&v);
    MDIT_ASSERT_EQ_SZ(mdit_vec_kvv_len(&v), 0);
    /* Re-pushing after clear must work and reuse the buffer. */
    kv *slot = mdit_vec_kvv_push(&v, (kv){.k = 7, .v = 99});
    MDIT_ASSERT_NE(slot, NULL);
    MDIT_ASSERT_EQ_INT(slot->k, 7);
    MDIT_ASSERT_EQ_INT(slot->v, 99);
    mdit_vec_kvv_destroy(&v);
}

MDIT_TEST(vec_reserve_does_not_truncate)
{
    mdit_vec_intv v;
    mdit_vec_intv_init(&v, NULL, NULL);
    (void)mdit_vec_intv_push(&v, 1);
    (void)mdit_vec_intv_push(&v, 2);
    (void)mdit_vec_intv_push(&v, 3);
    MDIT_ASSERT_TRUE(mdit_vec_intv_reserve(&v, 256));
    MDIT_ASSERT_EQ_SZ(mdit_vec_intv_len(&v), 3);
    MDIT_ASSERT_EQ_INT(*mdit_vec_intv_at(&v, 1), 2);
    mdit_vec_intv_destroy(&v);
}

#define MDIT_TEST_REGISTRY                                                 \
    MDIT_TEST_LIST_ENTRY(vec_arena_push_grows),                            \
    MDIT_TEST_LIST_ENTRY(vec_arena_extends_in_place_when_possible),        \
    MDIT_TEST_LIST_ENTRY(vec_malloc_push_grows),                           \
    MDIT_TEST_LIST_ENTRY(vec_pop_clear_emplace),                           \
    MDIT_TEST_LIST_ENTRY(vec_reserve_does_not_truncate)

#include "mdit_test_main.h"
