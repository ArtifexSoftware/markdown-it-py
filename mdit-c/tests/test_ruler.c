/*
 * test_ruler.c — ordered rule registry.
 *
 * Tests focus on the upstream contract:
 *   - push / before / after / at preserve declared ordering.
 *   - enable / disable / enable_only flip flags and invalidate cache.
 *   - get_rules('') returns the active default chain.
 *   - get_rules('alt') returns rules tagged with that alt label.
 *   - mutators after a get_rules() trigger a rebuild on next call.
 */
#include "mdit_test.h"

#include "arena.h"
#include "mdit/mdit.h"
#include "ruler.h"
#include "str.h"

#include <stddef.h>
#include <string.h>

/* Trivial rule fns: they tag a counter so tests can verify which
 * rules ran in which order. */
typedef struct call_log {
    int    seq[64];
    size_t len;
} call_log;

static bool rule_alpha(void *state, void *user) {
    call_log *log = (call_log *)state; (void)user;
    if (log->len < 64) log->seq[log->len++] = 1;
    return true;
}
static bool rule_beta(void *state, void *user) {
    call_log *log = (call_log *)state; (void)user;
    if (log->len < 64) log->seq[log->len++] = 2;
    return true;
}
static bool rule_gamma(void *state, void *user) {
    call_log *log = (call_log *)state; (void)user;
    if (log->len < 64) log->seq[log->len++] = 3;
    return true;
}
static bool rule_delta(void *state, void *user) {
    call_log *log = (call_log *)state; (void)user;
    if (log->len < 64) log->seq[log->len++] = 4;
    return true;
}

static void run(mdit_ruler *r, mdit_str chain, call_log *log)
{
    log->len = 0;
    size_t n = 0;
    const mdit_rule_entry *rules = mdit_ruler_get_rules(r, chain, &n);
    for (size_t i = 0; i < n; ++i) {
        rules[i].fn(log, rules[i].user);
    }
}

MDIT_TEST(ruler_push_preserves_order)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_ruler *r = mdit_ruler_new(&lib, &a);

    mdit_ruler_push(r, MDIT_STR_LIT("alpha"), rule_alpha, NULL,
                    MDIT_RULE_OPTIONS_NONE);
    mdit_ruler_push(r, MDIT_STR_LIT("beta"),  rule_beta,  NULL,
                    MDIT_RULE_OPTIONS_NONE);
    mdit_ruler_push(r, MDIT_STR_LIT("gamma"), rule_gamma, NULL,
                    MDIT_RULE_OPTIONS_NONE);

    call_log log = {0};
    run(r, MDIT_STR_LIT(""), &log);
    MDIT_ASSERT_EQ_SZ(log.len, 3);
    MDIT_ASSERT_EQ_INT(log.seq[0], 1);
    MDIT_ASSERT_EQ_INT(log.seq[1], 2);
    MDIT_ASSERT_EQ_INT(log.seq[2], 3);

    mdit_arena_destroy(&lib, &a);
}

MDIT_TEST(ruler_before_after_at)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_ruler *r = mdit_ruler_new(&lib, &a);

    mdit_ruler_push  (r, MDIT_STR_LIT("alpha"), rule_alpha, NULL,
                      MDIT_RULE_OPTIONS_NONE);
    mdit_ruler_push  (r, MDIT_STR_LIT("gamma"), rule_gamma, NULL,
                      MDIT_RULE_OPTIONS_NONE);
    /* Insert beta between them via `before(gamma)`. */
    mdit_ruler_before(r, MDIT_STR_LIT("gamma"), MDIT_STR_LIT("beta"),
                      rule_beta, NULL, MDIT_RULE_OPTIONS_NONE);
    /* Insert delta after gamma. */
    mdit_ruler_after (r, MDIT_STR_LIT("gamma"), MDIT_STR_LIT("delta"),
                      rule_delta, NULL, MDIT_RULE_OPTIONS_NONE);

    call_log log = {0};
    run(r, MDIT_STR_LIT(""), &log);
    MDIT_ASSERT_EQ_SZ(log.len, 4);
    MDIT_ASSERT_EQ_INT(log.seq[0], 1); /* alpha */
    MDIT_ASSERT_EQ_INT(log.seq[1], 2); /* beta */
    MDIT_ASSERT_EQ_INT(log.seq[2], 3); /* gamma */
    MDIT_ASSERT_EQ_INT(log.seq[3], 4); /* delta */

    /* `at` replaces the function in place, retaining position. */
    mdit_ruler_at(r, MDIT_STR_LIT("beta"), rule_alpha /* fn */, NULL,
                  MDIT_RULE_OPTIONS_NONE);
    run(r, MDIT_STR_LIT(""), &log);
    MDIT_ASSERT_EQ_SZ(log.len, 4);
    MDIT_ASSERT_EQ_INT(log.seq[1], 1); /* alpha now in beta's slot */

    mdit_arena_destroy(&lib, &a);
}

MDIT_TEST(ruler_disable_then_enable)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_ruler *r = mdit_ruler_new(&lib, &a);
    mdit_ruler_push(r, MDIT_STR_LIT("alpha"), rule_alpha, NULL,
                    MDIT_RULE_OPTIONS_NONE);
    mdit_ruler_push(r, MDIT_STR_LIT("beta"),  rule_beta,  NULL,
                    MDIT_RULE_OPTIONS_NONE);
    mdit_ruler_push(r, MDIT_STR_LIT("gamma"), rule_gamma, NULL,
                    MDIT_RULE_OPTIONS_NONE);

    mdit_str disable[] = { MDIT_STR_LIT("beta") };
    int n = mdit_ruler_disable(r, disable, 1, false);
    MDIT_ASSERT_EQ_INT(n, 1);

    call_log log = {0};
    run(r, MDIT_STR_LIT(""), &log);
    MDIT_ASSERT_EQ_SZ(log.len, 2);
    MDIT_ASSERT_EQ_INT(log.seq[0], 1);
    MDIT_ASSERT_EQ_INT(log.seq[1], 3);

    /* Re-enabling restores the rule and order. */
    mdit_str enable[] = { MDIT_STR_LIT("beta") };
    n = mdit_ruler_enable(r, enable, 1, false);
    MDIT_ASSERT_EQ_INT(n, 1);
    run(r, MDIT_STR_LIT(""), &log);
    MDIT_ASSERT_EQ_SZ(log.len, 3);
    MDIT_ASSERT_EQ_INT(log.seq[1], 2);

    mdit_arena_destroy(&lib, &a);
}

MDIT_TEST(ruler_disable_unknown_with_ignore)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_ruler *r = mdit_ruler_new(&lib, &a);
    mdit_ruler_push(r, MDIT_STR_LIT("alpha"), rule_alpha, NULL,
                    MDIT_RULE_OPTIONS_NONE);
    mdit_str names[] = { MDIT_STR_LIT("nope"), MDIT_STR_LIT("alpha") };
    int n = mdit_ruler_disable(r, names, 2, true);
    MDIT_ASSERT_EQ_INT(n, 1);    /* one toggled, one ignored */
    /* Without ignore, the same call should fail. */
    int n2 = mdit_ruler_disable(r, names, 2, false);
    MDIT_ASSERT_EQ_INT(n2, -1);
    mdit_arena_destroy(&lib, &a);
}

MDIT_TEST(ruler_enable_only_disables_others)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_ruler *r = mdit_ruler_new(&lib, &a);
    mdit_ruler_push(r, MDIT_STR_LIT("alpha"), rule_alpha, NULL,
                    MDIT_RULE_OPTIONS_NONE);
    mdit_ruler_push(r, MDIT_STR_LIT("beta"),  rule_beta,  NULL,
                    MDIT_RULE_OPTIONS_NONE);
    mdit_ruler_push(r, MDIT_STR_LIT("gamma"), rule_gamma, NULL,
                    MDIT_RULE_OPTIONS_NONE);

    mdit_str only[] = { MDIT_STR_LIT("gamma") };
    int n = mdit_ruler_enable_only(r, only, 1, false);
    MDIT_ASSERT_EQ_INT(n, 1);

    call_log log = {0};
    run(r, MDIT_STR_LIT(""), &log);
    MDIT_ASSERT_EQ_SZ(log.len, 1);
    MDIT_ASSERT_EQ_INT(log.seq[0], 3);

    mdit_arena_destroy(&lib, &a);
}

MDIT_TEST(ruler_alt_chains)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_ruler *r = mdit_ruler_new(&lib, &a);

    /* alpha is in the default chain only.
     * beta lives in 'alt1' and 'alt2'.
     * gamma lives in 'alt1'. */
    mdit_str alt_beta[]  = { MDIT_STR_LIT("alt1"), MDIT_STR_LIT("alt2") };
    mdit_str alt_gamma[] = { MDIT_STR_LIT("alt1") };

    mdit_rule_options opts_beta;
    opts_beta.alt = alt_beta; opts_beta.alt_len = 2;
    mdit_rule_options opts_gamma;
    opts_gamma.alt = alt_gamma; opts_gamma.alt_len = 1;

    mdit_ruler_push(r, MDIT_STR_LIT("alpha"), rule_alpha, NULL,
                    MDIT_RULE_OPTIONS_NONE);
    mdit_ruler_push(r, MDIT_STR_LIT("beta"),  rule_beta,  NULL, opts_beta);
    mdit_ruler_push(r, MDIT_STR_LIT("gamma"), rule_gamma, NULL, opts_gamma);

    call_log log = {0};
    /* default chain has all three. */
    run(r, MDIT_STR_LIT(""), &log);
    MDIT_ASSERT_EQ_SZ(log.len, 3);
    /* alt1: beta + gamma. */
    run(r, MDIT_STR_LIT("alt1"), &log);
    MDIT_ASSERT_EQ_SZ(log.len, 2);
    MDIT_ASSERT_EQ_INT(log.seq[0], 2);
    MDIT_ASSERT_EQ_INT(log.seq[1], 3);
    /* alt2: beta only. */
    run(r, MDIT_STR_LIT("alt2"), &log);
    MDIT_ASSERT_EQ_SZ(log.len, 1);
    MDIT_ASSERT_EQ_INT(log.seq[0], 2);
    /* unknown chain: empty. */
    size_t n = 999;
    const mdit_rule_entry *e = mdit_ruler_get_rules(r,
        MDIT_STR_LIT("does_not_exist"), &n);
    MDIT_ASSERT_EQ_PTR(e, NULL);
    MDIT_ASSERT_EQ_SZ(n, 0);

    mdit_arena_destroy(&lib, &a);
}

MDIT_TEST(ruler_duplicate_push_fails)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_ruler *r = mdit_ruler_new(&lib, &a);
    mdit_rule_status s1 = mdit_ruler_push(r, MDIT_STR_LIT("alpha"),
        rule_alpha, NULL, MDIT_RULE_OPTIONS_NONE);
    MDIT_ASSERT(s1.index >= 0);
    mdit_rule_status s2 = mdit_ruler_push(r, MDIT_STR_LIT("alpha"),
        rule_beta, NULL, MDIT_RULE_OPTIONS_NONE);
    MDIT_ASSERT_EQ_INT(s2.index, MDIT_RULE_DUPLICATE);
    MDIT_ASSERT_NE(s2.message, NULL);
    mdit_arena_destroy(&lib, &a);
}

MDIT_TEST(ruler_anchor_not_found)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_ruler *r = mdit_ruler_new(&lib, &a);
    mdit_rule_status s = mdit_ruler_before(r, MDIT_STR_LIT("missing"),
        MDIT_STR_LIT("alpha"), rule_alpha, NULL, MDIT_RULE_OPTIONS_NONE);
    MDIT_ASSERT_EQ_INT(s.index, MDIT_RULE_NOT_FOUND);
    mdit_arena_destroy(&lib, &a);
}

MDIT_TEST(ruler_introspection_apis)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a; mdit_arena_init(&a, 0);
    mdit_ruler *r = mdit_ruler_new(&lib, &a);
    mdit_ruler_push(r, MDIT_STR_LIT("alpha"), rule_alpha, NULL,
                    MDIT_RULE_OPTIONS_NONE);
    mdit_ruler_push(r, MDIT_STR_LIT("beta"),  rule_beta,  NULL,
                    MDIT_RULE_OPTIONS_NONE);
    mdit_ruler_push(r, MDIT_STR_LIT("gamma"), rule_gamma, NULL,
                    MDIT_RULE_OPTIONS_NONE);

    size_t n = 0;
    const mdit_str *all = mdit_ruler_all_rule_names(r, &n);
    MDIT_ASSERT_EQ_SZ(n, 3);
    MDIT_ASSERT_TRUE(mdit_str_eq_z(all[0], "alpha"));
    MDIT_ASSERT_TRUE(mdit_str_eq_z(all[2], "gamma"));

    mdit_str disable[] = { MDIT_STR_LIT("beta") };
    mdit_ruler_disable(r, disable, 1, false);

    const mdit_str *active = mdit_ruler_active_rule_names(r, &n);
    MDIT_ASSERT_EQ_SZ(n, 2);
    MDIT_ASSERT_TRUE(mdit_str_eq_z(active[0], "alpha"));
    MDIT_ASSERT_TRUE(mdit_str_eq_z(active[1], "gamma"));

    MDIT_ASSERT_TRUE (mdit_ruler_has(r, MDIT_STR_LIT("alpha")));
    MDIT_ASSERT_FALSE(mdit_ruler_has(r, MDIT_STR_LIT("missing")));

    mdit_arena_destroy(&lib, &a);
}

#define MDIT_TEST_REGISTRY                                                 \
    MDIT_TEST_LIST_ENTRY(ruler_push_preserves_order),                      \
    MDIT_TEST_LIST_ENTRY(ruler_before_after_at),                           \
    MDIT_TEST_LIST_ENTRY(ruler_disable_then_enable),                       \
    MDIT_TEST_LIST_ENTRY(ruler_disable_unknown_with_ignore),               \
    MDIT_TEST_LIST_ENTRY(ruler_enable_only_disables_others),               \
    MDIT_TEST_LIST_ENTRY(ruler_alt_chains),                                \
    MDIT_TEST_LIST_ENTRY(ruler_duplicate_push_fails),                      \
    MDIT_TEST_LIST_ENTRY(ruler_anchor_not_found),                          \
    MDIT_TEST_LIST_ENTRY(ruler_introspection_apis)

#include "mdit_test_main.h"
