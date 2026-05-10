/*
 * mdit_lib_ctx — library runtime context (allocators, OOM, future host hooks).
 *
 * Passed explicitly into APIs that need it (planned: arena and other
 * allocation sites). Not stored on mdit_arena.
 *
 * Include via <mdit/mdit.h> or use this header alone (MDIT_API defaults
 * to empty unless <mdit/mdit.h> was included first on Windows DLL builds).
 */
#ifndef MDIT_LIB_CTX_H
#define MDIT_LIB_CTX_H

#include <stddef.h>

#ifndef MDIT_API
#  define MDIT_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*mdit_lib_oom_fn)(void *user, size_t requested);
typedef void *(*mdit_lib_alloc_fn)(void *user, size_t size);
typedef void (*mdit_lib_free_fn)(void *user, void *ptr);

/*
 * Host-provided allocation hooks. `alloc` must return NULL on failure
 * (the library then calls `oom`, which must not return in normal use).
 * `user` is forwarded to every hook.
 *
 * Optional `realloc` may be added later; arenas will use alloc/free until then.
 */
typedef struct mdit_lib_ctx {
    void               *user;
    mdit_lib_alloc_fn   alloc;
    mdit_lib_free_fn    free_fn;
    mdit_lib_oom_fn     oom;
} mdit_lib_ctx;

/* libc malloc/free + stderr/abort OOM; user = NULL. */
MDIT_API void mdit_lib_ctx_init_defaults(mdit_lib_ctx *ctx);

/* Shared process-wide defaults (read-only hooks). Safe to copy from. */
MDIT_API const mdit_lib_ctx *mdit_lib_ctx_builtin_default(void);

#ifdef __cplusplus
}
#endif

#endif /* MDIT_LIB_CTX_H */
