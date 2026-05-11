/*
 * mdit_lib_ctx — default libc-backed runtime context.
 */
#include "mdit/mdit.h"

#include <stdio.h>
#include <stdlib.h>

static void mdit_lib_ctx_default_oom(void *user, size_t requested)
{
    (void)user;
    (void)fprintf(stderr,
                  "mdit-c: allocation failed (requested %zu bytes)\n",
                  requested);
    abort();
}

static void *mdit_lib_ctx_default_alloc(void *user, size_t size)
{
    (void)user;
    return malloc(size);
}

static void *mdit_lib_ctx_default_realloc(void *user, void *ptr, size_t size)
{
    (void)user;
    return realloc(ptr, size);
}

static void mdit_lib_ctx_default_free(void *user, void *ptr)
{
    (void)user;
    free(ptr);
}

static const mdit_lib_ctx k_mdit_lib_ctx_builtin_default = {
    .user       = NULL,
    .alloc      = mdit_lib_ctx_default_alloc,
    .realloc_fn = mdit_lib_ctx_default_realloc,
    .free_fn    = mdit_lib_ctx_default_free,
    .oom        = mdit_lib_ctx_default_oom,
};

const mdit_lib_ctx *mdit_lib_ctx_builtin_default(void)
{
    return &k_mdit_lib_ctx_builtin_default;
}

void mdit_lib_ctx_init_defaults(mdit_lib_ctx *ctx)
{
    if (ctx == NULL) {
        return;
    }
    *ctx = *mdit_lib_ctx_builtin_default();
}
