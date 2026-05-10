/*
 * test_map.c — unit tests for the ordered string-keyed map.
 */
#include "mdit_test.h"

#include "arena.h"
#include "map.h"
#include "mdit/mdit_lib_ctx.h"
#include "str.h"

#include <string.h>

MDIT_TEST(map_set_get_basic)
{
    mdit_map m;
    mdit_map_init(&m, NULL, NULL);

    MDIT_ASSERT_TRUE(mdit_map_set_z(&m, "href", mdit_value_cstr("/url")));
    MDIT_ASSERT_TRUE(mdit_map_set_z(&m, "title", mdit_value_cstr("hello")));

    const mdit_value *v = mdit_map_get_z(&m, "href");
    MDIT_ASSERT_NE(v, NULL);
    MDIT_ASSERT_EQ_INT(v->kind, MDIT_VALUE_STR);
    MDIT_ASSERT_TRUE(mdit_str_eq_z(v->u.s, "/url"));

    v = mdit_map_get_z(&m, "title");
    MDIT_ASSERT_NE(v, NULL);
    MDIT_ASSERT_TRUE(mdit_str_eq_z(v->u.s, "hello"));

    MDIT_ASSERT_EQ_PTR(mdit_map_get_z(&m, "missing"), NULL);

    mdit_map_destroy(&m);
}

MDIT_TEST(map_overwrite_preserves_order)
{
    mdit_map m;
    mdit_map_init(&m, NULL, NULL);
    mdit_map_set_z(&m, "a", mdit_value_int(1));
    mdit_map_set_z(&m, "b", mdit_value_int(2));
    mdit_map_set_z(&m, "c", mdit_value_int(3));
    /* Overwrite 'b' -- order must be a, b, c. */
    mdit_map_set_z(&m, "b", mdit_value_int(20));
    MDIT_ASSERT_EQ_SZ(mdit_map_len(&m), 3);
    MDIT_ASSERT_TRUE(mdit_str_eq_z(mdit_map_at(&m, 0)->key, "a"));
    MDIT_ASSERT_TRUE(mdit_str_eq_z(mdit_map_at(&m, 1)->key, "b"));
    MDIT_ASSERT_TRUE(mdit_str_eq_z(mdit_map_at(&m, 2)->key, "c"));
    MDIT_ASSERT_EQ_INT(mdit_map_at(&m, 1)->value.u.i, 20);
    mdit_map_destroy(&m);
}

MDIT_TEST(map_delete_keeps_order)
{
    mdit_map m;
    mdit_map_init(&m, NULL, NULL);
    mdit_map_set_z(&m, "a", mdit_value_int(1));
    mdit_map_set_z(&m, "b", mdit_value_int(2));
    mdit_map_set_z(&m, "c", mdit_value_int(3));
    mdit_map_set_z(&m, "d", mdit_value_int(4));
    MDIT_ASSERT_TRUE(mdit_map_del(&m, MDIT_STR_LIT("b")));
    MDIT_ASSERT_EQ_SZ(mdit_map_len(&m), 3);
    MDIT_ASSERT_TRUE(mdit_str_eq_z(mdit_map_at(&m, 0)->key, "a"));
    MDIT_ASSERT_TRUE(mdit_str_eq_z(mdit_map_at(&m, 1)->key, "c"));
    MDIT_ASSERT_TRUE(mdit_str_eq_z(mdit_map_at(&m, 2)->key, "d"));
    MDIT_ASSERT_FALSE(mdit_map_del(&m, MDIT_STR_LIT("missing")));
    mdit_map_destroy(&m);
}

MDIT_TEST(map_arena_backed)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a;
    mdit_arena_init(&a, 0);
    mdit_map m;
    mdit_map_init(&m, &lib, &a);
    /* Push enough entries to force at least a couple of growths. */
    char keybuf[32];
    for (int i = 0; i < 64; ++i) {
        snprintf(keybuf, sizeof keybuf, "k%d", i);
        char *owned = mdit_arena_strdup(&lib, &a, keybuf);
        mdit_map_set_z(&m, owned, mdit_value_int(i));
    }
    MDIT_ASSERT_EQ_SZ(mdit_map_len(&m), 64);
    /* Spot-check a value. */
    snprintf(keybuf, sizeof keybuf, "k42");
    const mdit_value *v = mdit_map_get_z(&m, keybuf);
    MDIT_ASSERT_NE(v, NULL);
    MDIT_ASSERT_EQ_INT(v->u.i, 42);
    mdit_map_destroy(&m);
    mdit_arena_destroy(&lib, &a);
}

MDIT_TEST(map_value_kinds)
{
    mdit_map m;
    mdit_map_init(&m, NULL, NULL);
    mdit_map_set_z(&m, "n",  mdit_value_null());
    mdit_map_set_z(&m, "b",  mdit_value_bool(true));
    mdit_map_set_z(&m, "i",  mdit_value_int(-7));
    mdit_map_set_z(&m, "d",  mdit_value_double(2.5));
    mdit_map_set_z(&m, "s",  mdit_value_cstr("hi"));

    MDIT_ASSERT_EQ_INT(mdit_map_get_z(&m, "n")->kind, MDIT_VALUE_NULL);
    MDIT_ASSERT_EQ_INT(mdit_map_get_z(&m, "b")->u.b, 1);
    MDIT_ASSERT_EQ_INT(mdit_map_get_z(&m, "i")->u.i, -7);
    /* doubles compared via int reinterpretation -- sufficient for bit-exact. */
    MDIT_ASSERT(mdit_map_get_z(&m, "d")->u.d > 2.4 &&
                mdit_map_get_z(&m, "d")->u.d < 2.6);
    MDIT_ASSERT_TRUE(mdit_str_eq_z(mdit_map_get_z(&m, "s")->u.s, "hi"));

    mdit_map_destroy(&m);
}

#define MDIT_TEST_REGISTRY                                                 \
    MDIT_TEST_LIST_ENTRY(map_set_get_basic),                               \
    MDIT_TEST_LIST_ENTRY(map_overwrite_preserves_order),                   \
    MDIT_TEST_LIST_ENTRY(map_delete_keeps_order),                          \
    MDIT_TEST_LIST_ENTRY(map_arena_backed),                                \
    MDIT_TEST_LIST_ENTRY(map_value_kinds)

#include "mdit_test_main.h"
