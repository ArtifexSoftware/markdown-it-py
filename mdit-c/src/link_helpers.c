#include "link_helpers.h"

#include <string.h>

#include "escape.h"
#include "json.h"

static mdit_str arena_copy(mdit_lib_ctx *lib, mdit_arena *arena,
                           const char *data, size_t len)
{
    if (len == 0) return MDIT_STR_LIT("");
    char *buf = (char *)mdit_arena_alloc(lib, arena, len);
    memcpy(buf, data, len);
    return (mdit_str){ buf, len };
}

static bool unescape_to_arena(mdit_lib_ctx *lib, mdit_arena *arena,
                              mdit_str input, mdit_str *out)
{
    mdit_buf b;
    mdit_buf_init(&b);
    bool ok = mdit_unescape_all(input, &b);
    if (ok) *out = arena_copy(lib, arena, b.data ? b.data : "", b.len);
    mdit_buf_destroy(&b);
    return ok;
}

bool mdit_parse_link_destination(mdit_lib_ctx *lib, mdit_arena *arena,
                                 mdit_str input,
                                 size_t pos,
                                 size_t maximum,
                                 mdit_link_destination_result *out)
{
    memset(out, 0, sizeof *out);
    if (pos >= maximum) return true;

    size_t start = pos;
    if (input.data[pos] == '<') {
        ++pos;
        while (pos < maximum) {
            unsigned char code = (unsigned char)input.data[pos];
            if (code == '\n') return true;
            if (code == '<') return true;
            if (code == '>') {
                out->pos = pos + 1;
                out->ok = unescape_to_arena(
                    lib, arena,
                    (mdit_str){ input.data + start + 1, pos - start - 1 },
                    &out->str);
                return out->ok;
            }
            if (code == '\\' && pos + 1 < maximum) {
                pos += 2;
                continue;
            }
            ++pos;
        }
        return true;
    }

    int32_t level = 0;
    while (pos < maximum) {
        unsigned char code = (unsigned char)input.data[pos];
        if (code == 0x20) break;
        if (code < 0x20 || code == 0x7F) break;
        if (code == '\\' && pos + 1 < maximum) {
            if (input.data[pos + 1] == 0x20) break;
            pos += 2;
            continue;
        }
        if (code == '(') {
            ++level;
            if (level > 32) return true;
        }
        if (code == ')') {
            if (level == 0) break;
            --level;
        }
        ++pos;
    }

    if (start == pos) return true;
    if (level != 0) return true;

    out->pos = pos;
    out->ok = unescape_to_arena(
        lib, arena,
        (mdit_str){ input.data + start, pos - start },
        &out->str);
    return out->ok;
}

bool mdit_parse_link_title(mdit_lib_ctx *lib, mdit_arena *arena,
                           mdit_str input,
                           size_t start,
                           size_t maximum,
                           const mdit_link_title_result *prev,
                           mdit_link_title_result *out)
{
    memset(out, 0, sizeof *out);
    size_t pos = start;
    uint32_t marker = 0;
    mdit_str prefix = MDIT_STR_LIT("");

    if (prev != NULL) {
        prefix = prev->str;
        marker = prev->marker;
    } else {
        if (pos >= maximum) return true;
        marker = (unsigned char)input.data[pos];
        if (marker != '"' && marker != '\'' && marker != '(') return true;
        ++start;
        ++pos;
        if (marker == '(') marker = ')';
    }
    out->marker = marker;

    while (pos < maximum) {
        unsigned char code = (unsigned char)input.data[pos];
        if (code == marker) {
            mdit_buf b;
            mdit_buf_init(&b);
            bool ok = true;
            if (prefix.len > 0) {
                ok = mdit_buf_append(&b, prefix.data, prefix.len);
            }
            if (ok) {
                ok = mdit_unescape_all(
                    (mdit_str){ input.data + start, pos - start }, &b);
            }
            if (ok) out->str = arena_copy(lib, arena, b.data ? b.data : "", b.len);
            mdit_buf_destroy(&b);
            if (!ok) return false;
            out->pos = pos + 1;
            out->ok = true;
            return true;
        }
        if (code == '(' && marker == ')') return true;
        if (code == '\\' && pos + 1 < maximum) ++pos;
        ++pos;
    }

    mdit_buf b;
    mdit_buf_init(&b);
    bool ok = true;
    if (prefix.len > 0) ok = mdit_buf_append(&b, prefix.data, prefix.len);
    if (ok) {
        ok = mdit_unescape_all(
            (mdit_str){ input.data + start, pos - start }, &b);
    }
    if (ok) out->str = arena_copy(lib, arena, b.data ? b.data : "", b.len);
    mdit_buf_destroy(&b);
    if (!ok) return false;
    out->can_continue = true;
    out->marker = marker;
    return true;
}
