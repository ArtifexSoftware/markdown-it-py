/*
 * lib_alloc.h — thin wrappers around mdit_lib_ctx allocation hooks.
 */
#ifndef MDIT_SRC_LIB_ALLOC_H
#define MDIT_SRC_LIB_ALLOC_H

#include <stddef.h>

#include "mdit/mdit.h"

static inline void *mdit_lib_alloc_bytes(mdit_lib_ctx *lib, size_t nbytes)
{
    if (lib == NULL || lib->alloc == NULL) {
        return NULL;
    }
    return lib->alloc(lib->user, nbytes);
}

static inline void *mdit_lib_realloc_bytes(mdit_lib_ctx *lib, void *ptr,
                                           size_t nbytes)
{
    if (lib == NULL || lib->realloc_fn == NULL) {
        return NULL;
    }
    return lib->realloc_fn(lib->user, ptr, nbytes);
}

static inline void mdit_lib_free_bytes(mdit_lib_ctx *lib, void *ptr)
{
    if (lib == NULL || ptr == NULL || lib->free_fn == NULL) {
        return;
    }
    lib->free_fn(lib->user, ptr);
}

#endif /* MDIT_SRC_LIB_ALLOC_H */
