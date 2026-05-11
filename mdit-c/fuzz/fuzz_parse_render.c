/*
 * fuzz_parse_render.c — exercise the full parse + render pipeline.
 *
 * Mirrors the upstream Python ``tests/fuzz/fuzz_markdown.py`` harness:
 * given an arbitrary byte buffer, run it through ``mdit_md_render``
 * and assert the engine never crashes regardless of the input shape.
 * The output is discarded — coverage / sanitizer signals are what
 * matter.
 *
 * Block + inline + core rules are all enabled (default preset). The
 * harness intentionally does NOT call ``mdit_md_init`` per input —
 * the engine is reused across invocations to surface accumulator
 * bugs and to keep per-input overhead low.
 */
#include "fuzz_common.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

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
    if (!mdit_md_init(&g_md, &g_lib, &g_arena)) {
        /* Initialisation failure is a hard build/config bug; not a
         * fuzz finding. Print and let the next call crash hard. */
        return;
    }
    initialised = 1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    engine_init_once();
    if (!initialised) return 0;

    mdit_str src = { (const char *)data, size };
    mdit_buf out;
    mdit_buf_init_default(&out);
    (void)mdit_md_render(&g_md, src, NULL, &out);
    mdit_buf_destroy(&out);
    return 0;
}
