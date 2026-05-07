#ifndef MDIT_SRC_NORMALIZE_URL_H
#define MDIT_SRC_NORMALIZE_URL_H

#include <stdbool.h>

#include "arena.h"
#include "str.h"

#ifdef __cplusplus
extern "C" {
#endif

bool mdit_normalize_link     (mdit_arena *arena, mdit_str url, mdit_str *out);
bool mdit_normalize_link_text(mdit_arena *arena, mdit_str url, mdit_str *out);
bool mdit_validate_link      (mdit_str url);

#ifdef __cplusplus
}
#endif

#endif /* MDIT_SRC_NORMALIZE_URL_H */
