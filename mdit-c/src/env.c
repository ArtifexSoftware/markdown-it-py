#include "env.h"

#include <string.h>

#include "case_fold.h"
#include "str.h"

void mdit_env_init(mdit_env *env, mdit_lib_ctx *lib, mdit_arena *arena)
{
    memset(env, 0, sizeof *env);
    env->lib   = lib;
    env->arena = arena;
}

static bool ref_eq(mdit_str a, mdit_str b)
{
    return a.len == b.len && (a.len == 0 || memcmp(a.data, b.data, a.len) == 0);
}

const mdit_reference *mdit_env_get_reference(const mdit_env *env,
                                             mdit_str label)
{
    if (env == NULL) return NULL;
    for (size_t i = 0; i < env->references_len; ++i) {
        if (ref_eq(env->references[i].label, label)) return &env->references[i];
    }
    return NULL;
}

static bool ensure_ref_cap(mdit_lib_ctx *lib, mdit_arena *arena,
                           mdit_reference **data,
                           size_t *cap,
                           size_t want)
{
    if (want <= *cap) return true;
    size_t new_cap = (*cap == 0) ? 8 : (*cap * 2);
    while (new_cap < want) new_cap *= 2;
    mdit_reference *next =
        (mdit_reference *)mdit_arena_alloc(lib, arena, new_cap * sizeof **data);
    if (*data != NULL && *cap > 0) {
        memcpy(next, *data, *cap * sizeof **data);
    }
    *data = next;
    *cap = new_cap;
    return true;
}

bool mdit_env_add_reference(mdit_env *env,
                            mdit_str label,
                            mdit_str href,
                            mdit_str title,
                            int32_t map_begin,
                            int32_t map_end)
{
    if (env == NULL) return true;
    mdit_reference ref;
    ref.label = label;
    ref.href = href;
    ref.title = title;
    ref.map_begin = map_begin;
    ref.map_end = map_end;

    if (mdit_env_get_reference(env, label) != NULL) {
        if (!ensure_ref_cap(env->lib, env->arena, &env->duplicate_refs,
                            &env->duplicate_refs_cap,
                            env->duplicate_refs_len + 1)) return false;
        env->duplicate_refs[env->duplicate_refs_len++] = ref;
        return true;
    }

    if (!ensure_ref_cap(env->lib, env->arena, &env->references,
                        &env->references_cap,
                        env->references_len + 1)) return false;
    env->references[env->references_len++] = ref;
    return true;
}

/* Mirror upstream `normalizeReference`: trim outer whitespace,
 * collapse runs of whitespace to a single space, then apply Python's
 * per-codepoint ``str.lower().upper()`` case-fold (see
 * ``case_fold.h``).
 *
 * Whitespace here is the same set Python's ``re.sub(r"\s+", " ",
 * s.strip())`` matches — that is, ASCII control whitespace. We do
 * *not* recognize the Unicode-only Zs / Zl / Zp categories: ``re``'s
 * ``\s`` only escalates beyond ASCII when the pattern is compiled
 * with ``re.UNICODE``-mapped escapes, which upstream's reference
 * normalizer does not request. (This matches the byte semantics of
 * markdown-it-py for every test corpus we mirror.) */
static bool ref_norm_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\v' || c == '\f';
}

mdit_str mdit_env_normalize_reference(mdit_lib_ctx *lib, mdit_arena *arena,
                                      mdit_str input)
{
    size_t lo = 0, hi = input.len;
    while (lo < hi && ref_norm_space((unsigned char)input.data[lo])) ++lo;
    while (hi > lo && ref_norm_space((unsigned char)input.data[hi - 1])) --hi;
    if (lo == hi) return MDIT_STR_LIT("");

    /* Worst case: every codepoint maps to a 6-byte UTF-8 sequence
     * (3-byte input -> 6-byte output, e.g. ﬃ U+FB03 -> ``FFI``). The
     * 2x bound is wasteful for the common ASCII-only path but keeps
     * the allocator call count down. */
    size_t cap = (hi - lo) * 2 + 1;
    char *buf = (char *)mdit_arena_alloc(lib, arena, cap);
    size_t w = 0;
    bool in_ws = false;

    size_t i = lo;
    while (i < hi) {
        unsigned char c0 = (unsigned char)input.data[i];
        if (ref_norm_space(c0)) {
            in_ws = true;
            ++i;
            continue;
        }
        if (in_ws && w > 0) buf[w++] = ' ';
        in_ws = false;

        /* Fast path for ASCII letters (the common case). */
        if (c0 < 0x80u) {
            if (c0 >= 'a' && c0 <= 'z') c0 = (unsigned char)(c0 - 32);
            buf[w++] = (char)c0;
            ++i;
            continue;
        }

        /* Non-ASCII: decode one codepoint and emit its case-fold
         * mapping (or pass through verbatim if no entry exists). */
        uint32_t cp = 0;
        size_t   step = mdit_decode(input.data + i, hi - i, &cp);
        if (step == 0) {
            /* Defensive: ``mdit_decode`` always advances by at least
             * 1 byte when input is non-empty; bail out to avoid an
             * infinite loop on malformed UTF-8. */
            buf[w++] = (char)c0;
            ++i;
            continue;
        }
        const char *m_bytes = NULL;
        size_t      m_len   = 0;
        if (mdit_case_fold_lookup(cp, &m_bytes, &m_len) && m_len > 0) {
            /* Resize if the worst-case bound underestimates (only
             * happens when many 1-byte ASCII chars map to multi-byte
             * outputs — shouldn't occur in practice, but be safe). */
            if (w + m_len > cap) {
                size_t new_cap = cap * 2 + m_len;
                char *grow = (char *)mdit_arena_alloc(lib, arena, new_cap);
                memcpy(grow, buf, w);
                buf = grow;
                cap = new_cap;
            }
            memcpy(buf + w, m_bytes, m_len);
            w += m_len;
        } else {
            /* Identity mapping: emit the original UTF-8 bytes. */
            if (w + step > cap) {
                size_t new_cap = cap * 2 + step;
                char *grow = (char *)mdit_arena_alloc(lib, arena, new_cap);
                memcpy(grow, buf, w);
                buf = grow;
                cap = new_cap;
            }
            memcpy(buf + w, input.data + i, step);
            w += step;
        }
        i += step;
    }
    return (mdit_str){ buf, w };
}
