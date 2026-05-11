/*
 * mdit.c — public ``mdit.h`` facade over the internal engine.
 */
#include "mdit/mdit.h"

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "linkifier.h"
#include "main.h"
#include "ruler.h"
#include "str.h"
#include "token.h"

#define MDIT_STRINGIFY_(x) #x
#define MDIT_STRINGIFY(x)  MDIT_STRINGIFY_(x)

static const char k_mdit_version[] =
    "mdit-c " MDIT_STRINGIFY(MDIT_VERSION_MAJOR) "."
    MDIT_STRINGIFY(MDIT_VERSION_MINOR) "."
    MDIT_STRINGIFY(MDIT_VERSION_PATCH);

struct mdit_tokens {
    const mdit_token *data;
    size_t            len;
};

struct mdit_ctx {
    mdit_lib_ctx   lib;
    mdit_arena     arena;
    mdit_md        engine;
    mdit_vec_token last_tokens;
    bool           tokens_ready;
    mdit_tokens    view;
};

static size_t mdit_src_len(const char *src, size_t n)
{
    if (src == NULL) {
        return 0;
    }
    if (n == SIZE_MAX) {
        return strlen(src);
    }
    return n;
}

static bool mdit_ctx_dup_str(mdit_ctx *ctx, const char *s, size_t n, mdit_str *out)
{
    if (n == 0 || s == NULL) {
        *out = mdit_str_make("", 0);
        return true;
    }
    char *buf = (char *)mdit_arena_alloc_aligned(&ctx->lib, &ctx->arena, n, 1);
    if (buf == NULL) {
        return false;
    }
    memcpy(buf, s, n);
    out->data = buf;
    out->len  = n;
    return true;
}

static void mdit_ensure_linkifier(mdit_ctx *ctx)
{
    if (ctx->engine.options.linkify && ctx->engine.linkifier == NULL) {
        mdit_md_set_linkifier(&ctx->engine, mdit_linkifier_default());
    }
}

static bool mdit_apply_zero_preset(mdit_ctx *ctx)
{
    static const char *const block_keep[]   = { "paragraph" };
    static const char *const inline_keep[]  = { "text" };
    static const char *const inline2_keep[] = { "balance_pairs", "fragments_join" };
    static const char *const core_keep[]    = { "normalize", "block", "inline", "text_join" };

    const struct {
        mdit_ruler          *r;
        const char *const   *keep;
        size_t               n_keep;
    } rulers[] = {
        { ctx->engine.block.ruler,       block_keep,   1 },
        { ctx->engine.inline_p.ruler,    inline_keep,  1 },
        { ctx->engine.inline_p.ruler2, inline2_keep, 2 },
        { ctx->engine.core.ruler,        core_keep,    4 },
    };

    for (size_t i = 0; i < sizeof rulers / sizeof *rulers; ++i) {
        size_t n = 0;
        const mdit_str *names = mdit_ruler_all_rule_names(rulers[i].r, &n);
        for (size_t j = 0; j < n; ++j) {
            bool keep = false;
            for (size_t k = 0; k < rulers[i].n_keep; ++k) {
                if (mdit_str_eq_z(names[j], rulers[i].keep[k])) {
                    keep = true;
                    break;
                }
            }
            if (!keep) {
                mdit_str name = names[j];
                (void)mdit_ruler_disable(rulers[i].r, &name, 1, true);
            }
        }
    }
    return true;
}

static bool mdit_apply_preset(mdit_ctx *ctx, const char *preset)
{
    if (preset == NULL || strcmp(preset, "default") == 0) {
        return true;
    }

    if (strcmp(preset, "commonmark") == 0) {
        ctx->engine.options.max_nesting = 20;
        ctx->engine.options.html        = true;
        ctx->engine.options.xhtml_out   = true;

        mdit_str table_name = MDIT_STR_LIT("table");
        mdit_str inline_optional[] = {
            MDIT_STR_LIT("linkify"),
            MDIT_STR_LIT("strikethrough"),
        };
        mdit_str core_optional[] = {
            MDIT_STR_LIT("linkify"),
            MDIT_STR_LIT("replacements"),
            MDIT_STR_LIT("smartquotes"),
        };
        (void)mdit_ruler_disable(ctx->engine.block.ruler, &table_name, 1, true);
        (void)mdit_ruler_disable(ctx->engine.inline_p.ruler, inline_optional,
                                 sizeof inline_optional / sizeof *inline_optional,
                                 true);
        (void)mdit_ruler_disable(ctx->engine.inline_p.ruler2, &inline_optional[1],
                                 1, true);
        (void)mdit_ruler_disable(ctx->engine.core.ruler, core_optional,
                                 sizeof core_optional / sizeof *core_optional,
                                 true);
        return true;
    }

    if (strcmp(preset, "zero") == 0) {
        return mdit_apply_zero_preset(ctx);
    }

    return false;
}

static mdit_status mdit_ctx_begin_parse(mdit_ctx *ctx)
{
    if (ctx == NULL) {
        return MDIT_ERR_INVALID_ARG;
    }
    if (ctx->tokens_ready) {
        mdit_vec_token_destroy(&ctx->last_tokens);
        ctx->tokens_ready = false;
    }
    mdit_vec_token_init(&ctx->last_tokens, &ctx->lib, &ctx->arena);
    return MDIT_OK;
}

static mdit_ruler **mdit_ctx_rulers(mdit_ctx *ctx, size_t *out_n)
{
    static mdit_ruler *rulers[4];
    rulers[0] = ctx->engine.core.ruler;
    rulers[1] = ctx->engine.block.ruler;
    rulers[2] = ctx->engine.inline_p.ruler;
    rulers[3] = ctx->engine.inline_p.ruler2;
    *out_n = 4;
    return rulers;
}

const char *mdit_version_string(void)
{
    return k_mdit_version;
}

mdit_ctx *mdit_new(const mdit_lib_ctx *lib, const char *preset)
{
    mdit_lib_ctx defaults;
    const mdit_lib_ctx *hooks = lib;
    if (hooks == NULL) {
        mdit_lib_ctx_init_defaults(&defaults);
        hooks = &defaults;
    }
    if (hooks->alloc == NULL || hooks->realloc_fn == NULL ||
        hooks->free_fn == NULL || hooks->oom == NULL) {
        return NULL;
    }

    mdit_ctx *ctx = (mdit_ctx *)hooks->alloc(hooks->user, sizeof *ctx);
    if (ctx == NULL) {
        return NULL;
    }
    memset(ctx, 0, sizeof *ctx);
    ctx->lib = *hooks;

    mdit_arena_init(&ctx->arena, 0);
    if (!mdit_md_init(&ctx->engine, &ctx->lib, &ctx->arena)) {
        mdit_free(ctx);
        return NULL;
    }
    if (!mdit_apply_preset(ctx, preset)) {
        mdit_free(ctx);
        return NULL;
    }
    mdit_ensure_linkifier(ctx);
    return ctx;
}

void mdit_free(mdit_ctx *ctx)
{
    if (ctx == NULL) {
        return;
    }
    if (ctx->tokens_ready) {
        mdit_vec_token_destroy(&ctx->last_tokens);
    }
    mdit_md_destroy(&ctx->engine);
    mdit_arena_destroy(&ctx->lib, &ctx->arena);
    ctx->lib.free_fn(ctx->lib.user, ctx);
}

static mdit_status mdit_set_option_bool_key(mdit_ctx *ctx, const char *key, int value)
{
    typedef struct {
        const char *key;
        size_t      offset;
    } mdit_option_bool_key;

    static const mdit_option_bool_key keys[] = {
        { "html",                       offsetof(mdit_options, html) },
        { "xhtmlOut",                   offsetof(mdit_options, xhtml_out) },
        { "breaks",                     offsetof(mdit_options, breaks) },
        { "linkify",                    offsetof(mdit_options, linkify) },
        { "typographer",                offsetof(mdit_options, typographer) },
        { "strikethrough_single_tilde", offsetof(mdit_options, strikethrough_single_tilde) },
        { "tasklists",                  offsetof(mdit_options, tasklists) },
        { "tasklists_editable",         offsetof(mdit_options, tasklists_editable) },
        { "alerts",                     offsetof(mdit_options, alerts) },
    };

    for (size_t i = 0; i < sizeof keys / sizeof *keys; ++i) {
        if (strcmp(key, keys[i].key) != 0) {
            continue;
        }
        bool *slot = (bool *)((char *)&ctx->engine.options + keys[i].offset);
        *slot = value ? true : false;
        mdit_ensure_linkifier(ctx);
        return MDIT_OK;
    }
    return MDIT_ERR_UNKNOWN_NAME;
}

mdit_status mdit_set_option_bool(mdit_ctx *ctx, const char *key, int value)
{
    if (ctx == NULL || key == NULL) {
        return MDIT_ERR_INVALID_ARG;
    }
    return mdit_set_option_bool_key(ctx, key, value);
}

mdit_status mdit_set_option_str(mdit_ctx *ctx, const char *key, const char *value)
{
    if (ctx == NULL || key == NULL || value == NULL) {
        return MDIT_ERR_INVALID_ARG;
    }

    if (strcmp(key, "langPrefix") == 0) {
        mdit_str dst;
        if (!mdit_ctx_dup_str(ctx, value, strlen(value), &dst)) {
            return MDIT_ERR_OOM;
        }
        ctx->engine.options.lang_prefix = dst;
        return MDIT_OK;
    }

    if (strcmp(key, "maxNesting") == 0) {
        char *end = NULL;
        long v = strtol(value, &end, 10);
        if (end == value || (end != NULL && *end != '\0')) {
            return MDIT_ERR_INVALID_ARG;
        }
        ctx->engine.options.max_nesting = (int32_t)v;
        return MDIT_OK;
    }

    if (strcmp(key, "quotes") == 0) {
        size_t pos = 0;
        size_t len = strlen(value);
        for (int i = 0; i < 4; ++i) {
            uint32_t cp = 0;
            size_t consumed = mdit_decode(value + pos, len - pos, &cp);
            if (consumed == 0) {
                return MDIT_ERR_INVALID_ARG;
            }
            char utf8[4];
            size_t n = mdit_encode(cp, utf8, sizeof utf8);
            if (n == 0) {
                return MDIT_ERR_INVALID_ARG;
            }
            mdit_str dst;
            if (!mdit_ctx_dup_str(ctx, utf8, n, &dst)) {
                return MDIT_ERR_OOM;
            }
            ctx->engine.options.quotes[i] = dst;
            pos += consumed;
        }
        return MDIT_OK;
    }

    return MDIT_ERR_UNKNOWN_NAME;
}

static mdit_status mdit_toggle_rules(mdit_ctx *ctx,
                                     const char *const *names,
                                     size_t n,
                                     int enable,
                                     int ignore_invalid)
{
    size_t nrulers = 0;
    mdit_ruler **rulers = mdit_ctx_rulers(ctx, &nrulers);

    for (size_t i = 0; i < n; ++i) {
        if (names[i] == NULL) {
            return MDIT_ERR_INVALID_ARG;
        }
        mdit_str name = mdit_str_make(names[i], strlen(names[i]));
        bool matched = false;
        for (size_t j = 0; j < nrulers; ++j) {
            int rc = enable
                ? mdit_ruler_enable(rulers[j], &name, 1, true)
                : mdit_ruler_disable(rulers[j], &name, 1, true);
            if (rc > 0) {
                matched = true;
            }
        }
        if (!matched && !ignore_invalid) {
            return MDIT_ERR_UNKNOWN_NAME;
        }
    }
    return MDIT_OK;
}

mdit_status mdit_enable(mdit_ctx *ctx, const char *const *names, size_t n,
                        int ignore_invalid)
{
    if (ctx == NULL || (n > 0 && names == NULL)) {
        return MDIT_ERR_INVALID_ARG;
    }
    return mdit_toggle_rules(ctx, names, n, 1, ignore_invalid);
}

mdit_status mdit_disable(mdit_ctx *ctx, const char *const *names, size_t n,
                         int ignore_invalid)
{
    if (ctx == NULL || (n > 0 && names == NULL)) {
        return MDIT_ERR_INVALID_ARG;
    }
    return mdit_toggle_rules(ctx, names, n, 0, ignore_invalid);
}

mdit_status mdit_use(mdit_ctx *ctx, mdit_plugin_fn plugin, void *user)
{
    (void)user;
    if (ctx == NULL || plugin == NULL) {
        return MDIT_ERR_INVALID_ARG;
    }
    return MDIT_ERR_UNSUPPORTED;
}

mdit_status mdit_parse(mdit_ctx *ctx, const char *src, size_t n,
                       const mdit_tokens **out)
{
    if (ctx == NULL || out == NULL) {
        return MDIT_ERR_INVALID_ARG;
    }

    mdit_status st = mdit_ctx_begin_parse(ctx);
    if (st != MDIT_OK) {
        return st;
    }

    mdit_str input = mdit_str_make(src, mdit_src_len(src, n));
    if (!mdit_md_parse(&ctx->engine, input, NULL, &ctx->last_tokens)) {
        return MDIT_ERR_INTERNAL;
    }

    ctx->view.data = ctx->last_tokens.data;
    ctx->view.len  = ctx->last_tokens.len;
    ctx->tokens_ready = true;
    *out = &ctx->view;
    return MDIT_OK;
}

size_t mdit_tokens_len(const mdit_tokens *toks)
{
    if (toks == NULL) {
        return 0;
    }
    return toks->len;
}

const struct mdit_token *mdit_tokens_at(const mdit_tokens *toks, size_t i)
{
    if (toks == NULL || i >= toks->len) {
        return NULL;
    }
    return &toks->data[i];
}

mdit_status mdit_render(mdit_ctx *ctx, const char *src, size_t n,
                        char **out, size_t *out_len)
{
    if (ctx == NULL || out == NULL) {
        return MDIT_ERR_INVALID_ARG;
    }

    mdit_status st = mdit_ctx_begin_parse(ctx);
    if (st != MDIT_OK) {
        return st;
    }

    mdit_buf rendered;
    mdit_buf_init(&rendered, &ctx->lib);
    mdit_str input = mdit_str_make(src, mdit_src_len(src, n));
    if (!mdit_md_render(&ctx->engine, input, NULL, &rendered)) {
        mdit_buf_destroy(&rendered);
        return MDIT_ERR_INTERNAL;
    }

    size_t rendered_len = rendered.len;
    char *copy = (char *)ctx->lib.alloc(ctx->lib.user, rendered_len + 1);
    if (copy == NULL) {
        mdit_buf_destroy(&rendered);
        return MDIT_ERR_OOM;
    }
    if (rendered_len > 0) {
        memcpy(copy, rendered.data, rendered_len);
    }
    copy[rendered_len] = '\0';
    mdit_buf_destroy(&rendered);

    *out = copy;
    if (out_len != NULL) {
        *out_len = rendered_len;
    }
    return MDIT_OK;
}

void mdit_free_string(mdit_ctx *ctx, char *p)
{
    if (ctx == NULL || p == NULL) {
        return;
    }
    ctx->lib.free_fn(ctx->lib.user, p);
}
