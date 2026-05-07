#ifndef MDIT_SRC_ENV_H
#define MDIT_SRC_ENV_H

#include <stdbool.h>
#include <stddef.h>

#include "arena.h"
#include "str.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mdit_reference {
    mdit_str label;
    mdit_str href;
    mdit_str title;
    int32_t  map_begin;
    int32_t  map_end;
} mdit_reference;

typedef struct mdit_env {
    mdit_arena     *arena;
    mdit_reference *references;
    size_t          references_len;
    size_t          references_cap;
    mdit_reference *duplicate_refs;
    size_t          duplicate_refs_len;
    size_t          duplicate_refs_cap;
} mdit_env;

void mdit_env_init(mdit_env *env, mdit_arena *arena);

bool mdit_env_add_reference(mdit_env *env,
                            mdit_str label,
                            mdit_str href,
                            mdit_str title,
                            int32_t map_begin,
                            int32_t map_end);

const mdit_reference *mdit_env_get_reference(const mdit_env *env,
                                             mdit_str label);

/* Normalize a reference label per upstream `normalizeReference`:
 * trim, collapse whitespace, ASCII-fold to uppercase. The result is
 * arena-allocated. (Full Unicode case-folding is not yet implemented;
 * non-ASCII labels are matched verbatim after whitespace collapsing.)
 */
mdit_str mdit_env_normalize_reference(mdit_arena *arena, mdit_str input);

#ifdef __cplusplus
}
#endif

#endif /* MDIT_SRC_ENV_H */
