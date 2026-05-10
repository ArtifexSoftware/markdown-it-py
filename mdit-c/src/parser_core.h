/*
 * parser_core.h — top-level (core) chain.
 *
 * Holds an ``mdit_ruler`` of core rules (signature
 * ``void(mdit_state_core *)``). Built-in rules registered at init:
 * normalize → block → inline. Other upstream core rules
 * (linkify / replace / smartquotes / text_join) will be added later.
 */
#ifndef MDIT_SRC_PARSER_CORE_H
#define MDIT_SRC_PARSER_CORE_H

#include <stdbool.h>

#include "ruler.h"
#include "state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*mdit_core_rule_fn)(mdit_state_core *state);

typedef struct mdit_parser_core {
    mdit_ruler    *ruler;
    mdit_lib_ctx  *lib;
    mdit_arena    *arena;
} mdit_parser_core;

bool mdit_parser_core_init   (mdit_parser_core *p, mdit_lib_ctx *lib,
                              mdit_arena *arena);
void mdit_parser_core_destroy(mdit_parser_core *p);
void mdit_parser_core_process(mdit_parser_core *p, mdit_state_core *state);

#ifdef __cplusplus
}
#endif

#endif /* MDIT_SRC_PARSER_CORE_H */
