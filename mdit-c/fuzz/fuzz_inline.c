/*
 * fuzz_inline.c — exercise the inline-only pipeline.
 *
 * Bypasses block parsing and feeds the input straight to the inline
 * tokenizer + renderer (the path used by ``MarkdownIt.renderInline``).
 * Catches inline-rule crashes that the full pipeline would mask
 * because the block parser stripped or paragraph-wrapped the input.
 */
#include "fuzz_common.h"

#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "main.h"
#include "str.h"

static int initialised = 0;
static mdit_lib_ctx g_lib;
static mdit_arena   g_arena;
static mdit_md      g_md;

static void engine_init_once(void)
{
    if (initialised) return;
    mdit_lib_ctx_init_defaults(&g_lib);
    mdit_arena_init(&g_arena, 0);
    if (!mdit_md_init(&g_md, &g_lib, &g_arena)) return;
    initialised = 1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    engine_init_once();
    if (!initialised) return 0;

    mdit_str src = { (const char *)data, size };
    mdit_buf out;
    mdit_buf_init_default(&out);
    (void)mdit_md_render_inline(&g_md, src, NULL, &out);
    mdit_buf_destroy(&out);
    return 0;
}
