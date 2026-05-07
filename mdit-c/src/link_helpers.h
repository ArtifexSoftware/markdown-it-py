#ifndef MDIT_SRC_LINK_HELPERS_H
#define MDIT_SRC_LINK_HELPERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "str.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mdit_link_destination_result {
    bool     ok;
    size_t   pos;
    mdit_str str;
} mdit_link_destination_result;

typedef struct mdit_link_title_result {
    bool     ok;
    bool     can_continue;
    size_t   pos;
    mdit_str str;
    uint32_t marker;
} mdit_link_title_result;

bool mdit_parse_link_destination(mdit_arena *arena,
                                 mdit_str input,
                                 size_t pos,
                                 size_t maximum,
                                 mdit_link_destination_result *out);

bool mdit_parse_link_title(mdit_arena *arena,
                           mdit_str input,
                           size_t start,
                           size_t maximum,
                           const mdit_link_title_result *prev,
                           mdit_link_title_result *out);

#ifdef __cplusplus
}
#endif

#endif /* MDIT_SRC_LINK_HELPERS_H */
