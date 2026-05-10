/*
 * test_block_rules.c — drives the C parser over the auto-generated
 * `block_oracle.h` table and asserts byte-identical HTML output to
 * Python's MarkdownIt('commonmark').render().
 */
#include "mdit_test.h"

#include "arena.h"
#include "json.h"
#include "linkifier.h"
#include "main.h"
#include "mdit/mdit_lib_ctx.h"

#include "block_oracle.h"

static void run_case(const mdit_block_oracle_case *c)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a;
    mdit_arena_init(&a, 0);
    mdit_md md;
    if (!mdit_md_init(&md, &lib, &a)) {
        mdit_arena_destroy(&lib, &a);
        MDIT_FAIL("mdit_md_init");
    }
    if (c->opts & MDIT_BLOCK_ORACLE_OPT_HTML) md.options.html = true;
    if (c->opts & MDIT_BLOCK_ORACLE_OPT_LINKIFY) {
        md.options.linkify = true;
        mdit_md_set_linkifier(&md, mdit_linkifier_default());
    }
    if (c->opts & MDIT_BLOCK_ORACLE_OPT_TYPOGRAPHER) {
        md.options.typographer = true;
    }
    if (c->opts & MDIT_BLOCK_ORACLE_OPT_TASKLISTS) {
        md.options.tasklists = true;
    }
    if (c->opts & MDIT_BLOCK_ORACLE_OPT_TASKLISTS_EDITABLE) {
        md.options.tasklists = true;
        md.options.tasklists_editable = true;
    }
    if (c->opts & MDIT_BLOCK_ORACLE_OPT_ALERTS) {
        md.options.alerts = true;
    }
    /* The full-TLDs flag is process-wide on the default linkifier;
     * save/restore around each case so tests stay independent. */
    bool prev_full_tlds = mdit_linkifier_default_full_tlds_enabled();
    if (c->opts & MDIT_BLOCK_ORACLE_OPT_FULL_TLDS) {
        mdit_linkifier_default_use_full_tlds(true);
    } else {
        mdit_linkifier_default_use_full_tlds(false);
    }
    mdit_buf out;
    mdit_buf_init(&out);
    mdit_str src = { c->input, c->input_len };
    bool ok = mdit_md_render(&md, src, NULL, &out);
    if (!ok) {
        mdit_buf_destroy(&out);
        mdit_md_destroy(&md);
        mdit_arena_destroy(&lib, &a);
        MDIT_FAIL("mdit_md_render");
    }
    if (out.len != c->html_len ||
        memcmp(out.data ? out.data : "", c->html, c->html_len) != 0) {
        char msg[2048];
        snprintf(msg, sizeof msg,
                 "[%s]\n  input:  %.*s\n  got:    %.*s\n  want:   %.*s",
                 c->title,
                 (int)c->input_len,        c->input,
                 (int)out.len,             out.data ? out.data : "",
                 (int)c->html_len,         c->html);
        mdit_buf_destroy(&out);
        mdit_md_destroy(&md);
        mdit_arena_destroy(&lib, &a);
        MDIT_FAIL(msg);
    }
    mdit_buf_destroy(&out);
    mdit_md_destroy(&md);
    mdit_arena_destroy(&lib, &a);
    mdit_linkifier_default_use_full_tlds(prev_full_tlds);
}

MDIT_TEST(block_rules_match_python_oracle)
{
    for (size_t i = 0; i < MDIT_BLOCK_ORACLE_LEN; ++i) {
        run_case(&MDIT_BLOCK_ORACLE[i]);
    }
}

#define MDIT_TEST_REGISTRY \
    MDIT_TEST_LIST_ENTRY(block_rules_match_python_oracle)

#include "mdit_test_main.h"
