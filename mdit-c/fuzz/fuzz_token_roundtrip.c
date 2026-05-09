/*
 * fuzz_token_roundtrip.c — parse + JSON-serialise the token stream.
 *
 * Hits a different surface than ``fuzz_parse_render``: the JSON
 * serialiser (``mdit_tokens_to_json``) is the cross-check used by the
 * token oracle and is the natural place where escaping bugs in
 * ``mdit_token`` payload strings would show up. Fuzzing it
 * specifically protects the oracle contract.
 */
#include "fuzz_common.h"

#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "main.h"
#include "str.h"
#include "token.h"

static int initialised = 0;
static mdit_arena g_arena;
static mdit_md    g_md;

static void engine_init_once(void)
{
    if (initialised) return;
    mdit_arena_init(&g_arena, 0);
    if (!mdit_md_init(&g_md, &g_arena)) return;
    initialised = 1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    engine_init_once();
    if (!initialised) return 0;

    mdit_str src = { (const char *)data, size };
    mdit_vec_token tokens;
    mdit_vec_token_init(&tokens, g_md.arena);
    if (!mdit_md_parse(&g_md, src, NULL, &tokens)) {
        mdit_vec_token_destroy(&tokens);
        return 0;
    }
    mdit_buf json;
    mdit_buf_init(&json);
    (void)mdit_tokens_to_json(tokens.data, tokens.len, &json);
    mdit_buf_destroy(&json);
    mdit_vec_token_destroy(&tokens);
    return 0;
}
