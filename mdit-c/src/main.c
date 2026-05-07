/*
 * main.c — MarkdownIt facade.
 */
#include "main.h"

#include <string.h>

#include "env.h"

bool mdit_md_init(mdit_md *md, mdit_arena *arena)
{
    memset(md, 0, sizeof *md);
    md->arena = arena;
    mdit_options defaults = MDIT_OPTIONS_DEFAULTS;
    md->options = defaults;

    if (!mdit_parser_core_init(&md->core, arena))   return false;
    if (!mdit_parser_block_init(&md->block, arena)) return false;
    if (!mdit_parser_inline_init(&md->inline_p, arena)) return false;
    md->renderer = mdit_renderer_new(arena);
    if (md->renderer == NULL) return false;
    return true;
}

void mdit_md_destroy(mdit_md *md)
{
    mdit_parser_core_destroy(&md->core);
    mdit_parser_block_destroy(&md->block);
    mdit_parser_inline_destroy(&md->inline_p);
    mdit_renderer_destroy(md->renderer);
    md->renderer = NULL;
    md->linkifier = NULL;
}

void mdit_md_set_linkifier(mdit_md *md, const mdit_linkifier *linkifier)
{
    md->linkifier = linkifier;
}

bool mdit_md_parse(mdit_md *md, mdit_str src, void *env,
                   mdit_vec_token *out_tokens)
{
    mdit_env auto_env;
    if (env == NULL) {
        mdit_env_init(&auto_env, md->arena);
        env = &auto_env;
    }
    mdit_state_core state;
    mdit_state_core_init(&state, md->arena, src, md, env, out_tokens);
    mdit_parser_core_process(&md->core, &state);
    return true;
}

bool mdit_md_render(mdit_md *md, mdit_str src, void *env, mdit_buf *out)
{
    mdit_env auto_env;
    if (env == NULL) {
        mdit_env_init(&auto_env, md->arena);
        env = &auto_env;
    }
    mdit_vec_token tokens;
    mdit_vec_token_init(&tokens, md->arena);
    if (!mdit_md_parse(md, src, env, &tokens)) return false;

    mdit_renderer_options ropts;
    ropts.xhtmlOut   = md->options.xhtml_out;
    ropts.breaks     = md->options.breaks;
    ropts.langPrefix = md->options.lang_prefix;

    bool ok = mdit_renderer_render(md->renderer, tokens.data, tokens.len,
                                   &ropts, env, out);
    mdit_vec_token_destroy(&tokens);
    return ok;
}
