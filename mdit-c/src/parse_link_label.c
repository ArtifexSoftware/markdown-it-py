#include "parse_link_label.h"

ptrdiff_t mdit_parse_link_label(mdit_parser_inline *p,
                                mdit_state_inline *state,
                                size_t start,
                                bool disable_nested)
{
    size_t old_pos = state->pos;
    bool   found   = false;
    int    level   = 1;

    state->pos = start + 1;

    while (state->pos < state->pos_max) {
        unsigned char marker = (unsigned char)state->src.data[state->pos];
        if (marker == ']') {
            --level;
            if (level == 0) {
                found = true;
                break;
            }
        }

        size_t prev_pos = state->pos;
        mdit_parser_inline_skip_token(p, state);
        if (marker == '[') {
            if (prev_pos == state->pos - 1) {
                /* literal `[` byte advanced by one — increase nesting */
                ++level;
            } else if (disable_nested) {
                state->pos = old_pos;
                return -1;
            }
        }
    }

    ptrdiff_t label_end = -1;
    if (found) label_end = (ptrdiff_t)state->pos;
    state->pos = old_pos;
    return label_end;
}
