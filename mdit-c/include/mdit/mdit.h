/*
 * mdit-c — C99 port of markdown-it-py.
 *
 * Public API entry points. The header is intentionally small: tokens,
 * renderer hooks, and plugin/ruler types live in their own headers and
 * are pulled in here for convenience.
 *
 * Stability: PHASE 0 — the surface below is a draft. Anything marked
 * MDIT_API is the planned stable shape; expect minor naming churn until
 * Phase 5 ships.
 */
#ifndef MDIT_H
#define MDIT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(MDIT_SHARED)
#  ifdef MDIT_BUILDING
#    define MDIT_API __declspec(dllexport)
#  else
#    define MDIT_API __declspec(dllimport)
#  endif
#else
#  define MDIT_API
#endif

/* --- Library runtime context --------------------------------------------- */
/*
 * Host-provided allocation hooks. `alloc` must return NULL on failure
 * (the library then calls `oom`, which must not return in normal use).
 * `user` is forwarded to every hook.
 */
typedef void (*mdit_lib_oom_fn)(void *user, size_t requested);
typedef void *(*mdit_lib_alloc_fn)(void *user, size_t size);
typedef void (*mdit_lib_free_fn)(void *user, void *ptr);

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

/* --- Versioning ---------------------------------------------------------- */
#define MDIT_VERSION_MAJOR 0
#define MDIT_VERSION_MINOR 0
#define MDIT_VERSION_PATCH 1

MDIT_API const char *mdit_version_string(void);

/* --- Status codes -------------------------------------------------------- */
typedef enum mdit_status {
    MDIT_OK = 0,
    MDIT_ERR_OOM = -1,
    MDIT_ERR_INVALID_ARG = -2,
    MDIT_ERR_UNKNOWN_NAME = -3,   /* unknown rule / option name */
    MDIT_ERR_UNSUPPORTED = -4,    /* feature deliberately unimplemented */
    MDIT_ERR_INTERNAL = -99
} mdit_status;

/* --- Opaque types -------------------------------------------------------- */
typedef struct mdit_ctx     mdit_ctx;     /* a configured parser+renderer */
typedef struct mdit_tokens  mdit_tokens;  /* a token stream owned by an arena */
struct mdit_token;                        /* defined in mdit/token.h */

/* --- Lifecycle ----------------------------------------------------------- */
/*
 * Create a new parser context. `preset` selects the rule set; pass NULL or
 * "default" for the kitchen-sink config, "commonmark" for strict CommonMark,
 * or "zero" for the empty preset (only `text` + `paragraph` enabled).
 */
MDIT_API mdit_ctx *mdit_new(const char *preset);
MDIT_API void      mdit_free(mdit_ctx *md);

/* --- Configuration ------------------------------------------------------- */
MDIT_API mdit_status mdit_set_option_str(mdit_ctx *md, const char *key, const char *value);
MDIT_API mdit_status mdit_set_option_bool(mdit_ctx *md, const char *key, int value);

MDIT_API mdit_status mdit_enable (mdit_ctx *md, const char *const *names, size_t n,
                                  int ignore_invalid);
MDIT_API mdit_status mdit_disable(mdit_ctx *md, const char *const *names, size_t n,
                                  int ignore_invalid);

/* Plugin entry point: called once at registration to install rules. */
typedef mdit_status (*mdit_plugin_fn)(mdit_ctx *md, void *user);
MDIT_API mdit_status mdit_use(mdit_ctx *md, mdit_plugin_fn plugin, void *user);

/* --- Parsing & rendering ------------------------------------------------- */
/*
 * Parse `src` (UTF-8, length `n` bytes; pass SIZE_MAX for NUL-terminated)
 * and return an opaque token stream owned by `md`. The stream is valid
 * until the next mdit_parse / mdit_render / mdit_free call on `md`.
 */
MDIT_API mdit_status mdit_parse(mdit_ctx *md, const char *src, size_t n,
                                const mdit_tokens **out);

/* Number of top-level tokens in a stream. Children (inline tokens) are
 * reachable via the per-token API in token.h. */
MDIT_API size_t mdit_tokens_len(const mdit_tokens *toks);
MDIT_API const struct mdit_token *mdit_tokens_at(const mdit_tokens *toks, size_t i);

/*
 * Render `src` to HTML. On success, *out is a malloc()-allocated, NUL-
 * terminated buffer that the caller must free() (separate from the
 * arena because rendered output typically outlives the parse).
 */
MDIT_API mdit_status mdit_render(mdit_ctx *md, const char *src, size_t n,
                                 char **out, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* MDIT_H */
