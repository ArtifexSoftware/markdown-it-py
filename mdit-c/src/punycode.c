/*
 * punycode.c — RFC 3492 Bootstring (Punycode) and IDN domain helpers.
 *
 * The codec follows RFC 3492 to the letter, mirroring CPython's
 * ``codecs.encode/decode(s, "punycode")`` byte-for-byte (we cross-check
 * via the linkify oracle: when Python emits ``xn--d1abbgf6aiiy`` for a
 * Cyrillic label, the C side must too). Identifier names track the
 * RFC's pseudocode (``base``, ``tmin``, ``tmax``, ``skew``, ``damp``,
 * ``initial_n``, ``initial_bias``) so the algorithm stays auditable
 * against the spec.
 *
 * Overflow safety: ``delta`` and ``i`` are uint64_t even though the
 * RFC tolerates uint32_t. The bound for a 63-byte DNS label is well
 * within 32 bits, but oversizing keeps us safe against pathological
 * inputs from a Markdown source without burdening the hot path.
 */
#include "punycode.h"

#include <string.h>

#include "json.h"
#include "lib_alloc.h"

/* ---------------------------------------------------------------------
 * RFC 3492 Bootstring constants and helpers.
 * ------------------------------------------------------------------- */
enum {
    PC_BASE         = 36,
    PC_TMIN         = 1,
    PC_TMAX         = 26,
    PC_SKEW         = 38,
    PC_DAMP         = 700,
    PC_INITIAL_BIAS = 72,
    PC_INITIAL_N    = 0x80
};

/* RFC 3492 §5: digit_to_basic / basic_to_digit. */
static int pc_digit_to_basic(unsigned d, bool upper)
{
    /* 0..25 -> 'a'..'z' (or 'A'..'Z'); 26..35 -> '0'..'9'. */
    if (d < 26u) return (upper ? 'A' : 'a') + (int)d;
    return '0' + (int)(d - 26u);
}

static int pc_basic_to_digit(int c)
{
    if (c >= 'a' && c <= 'z') return c - 'a';
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= '0' && c <= '9') return c - '0' + 26;
    return -1;
}

/* RFC 3492 §6.1: bias adaptation. */
static unsigned pc_adapt(uint64_t delta, size_t numpoints, bool firsttime)
{
    delta = firsttime ? (delta / PC_DAMP) : (delta >> 1);
    delta += delta / (uint64_t)numpoints;

    unsigned k = 0;
    while (delta > ((uint64_t)(PC_BASE - PC_TMIN) * PC_TMAX) / 2u) {
        delta /= (uint64_t)(PC_BASE - PC_TMIN);
        k += PC_BASE;
    }
    return k + (unsigned)((((uint64_t)(PC_BASE - PC_TMIN + 1)) * delta) /
                          (delta + PC_SKEW));
}

/* ---------------------------------------------------------------------
 * Encode (codepoints -> Punycode ASCII).
 * Mirrors codecs.encode(s, "punycode") — including the trailing '-' the
 * CPython implementation always emits when basic codepoints were
 * present.
 * ------------------------------------------------------------------- */
bool mdit_punycode_encode_cps(const uint32_t *cps, size_t n, mdit_buf *out)
{
    /* Step 1: copy basic (ASCII) codepoints in order. */
    size_t basic_count = 0;
    for (size_t i = 0; i < n; ++i) {
        if (cps[i] < 0x80u) {
            if (!mdit_buf_append_byte(out, (char)(unsigned char)cps[i])) return false;
            ++basic_count;
        }
    }
    /* Step 2: '-' delimiter when any basic codepoints were emitted.
     * (CPython's punycode codec emits the delimiter after the basic
     * prefix, also for all-ASCII inputs.) */
    if (basic_count > 0) {
        if (!mdit_buf_append_byte(out, '-')) return false;
    }

    /* Step 3: encode the rest. */
    uint32_t n_value = PC_INITIAL_N;
    uint64_t delta   = 0;
    unsigned bias    = PC_INITIAL_BIAS;
    size_t   h       = basic_count;
    size_t   b       = basic_count;

    while (h < n) {
        /* Find the smallest non-basic codepoint >= n_value. */
        uint32_t m = 0xFFFFFFFFu;
        for (size_t i = 0; i < n; ++i) {
            if (cps[i] >= n_value && cps[i] < m) m = cps[i];
        }
        if (m == 0xFFFFFFFFu) return false;  /* malformed: no candidate */

        delta += (uint64_t)(m - n_value) * (uint64_t)(h + 1u);
        n_value = m;

        for (size_t i = 0; i < n; ++i) {
            uint32_t c = cps[i];
            if (c < n_value) {
                ++delta;
            } else if (c == n_value) {
                /* Encode delta as a generalized variable-length integer. */
                uint64_t q = delta;
                unsigned k = PC_BASE;
                for (;;) {
                    unsigned t;
                    if (k <= bias)                t = PC_TMIN;
                    else if (k >= bias + PC_TMAX) t = PC_TMAX;
                    else                          t = k - bias;
                    if (q < t) break;
                    unsigned digit = (unsigned)(t + ((q - t) % (uint64_t)(PC_BASE - t)));
                    if (!mdit_buf_append_byte(out, (char)pc_digit_to_basic(digit, false))) return false;
                    q = (q - t) / (uint64_t)(PC_BASE - t);
                    k += PC_BASE;
                }
                if (!mdit_buf_append_byte(out, (char)pc_digit_to_basic((unsigned)q, false))) return false;

                bias = pc_adapt(delta, h + 1u, h == b);
                delta = 0;
                ++h;
            }
        }
        ++delta;
        ++n_value;
    }
    return true;
}

/* ---------------------------------------------------------------------
 * Decode (Punycode ASCII -> codepoints, then UTF-8 into ``out``).
 * Mirrors codecs.decode(s, "punycode") — i.e. accepts upper/lowercase
 * digits, treats the *last* hyphen as the basic/extended delimiter,
 * rejects malformed integers.
 * ------------------------------------------------------------------- */
bool mdit_punycode_decode_to_utf8(mdit_str input, mdit_buf *out)
{
    /* Find the last hyphen. Bytes [0..last) are basic, [last+1..n) is
     * the extended part. If no hyphen, the entire input is extended. */
    size_t last = (size_t)-1;
    for (size_t i = 0; i < input.len; ++i) {
        if (input.data[i] == '-') last = i;
    }

    /* Validate basic codepoints (must all be ASCII < 0x80) and copy. */
    size_t basic_end = (last == (size_t)-1) ? 0u : last;
    /* Use a small dynamic buffer of decoded codepoints. */
    uint32_t *cps = NULL;
    size_t    cps_len = 0;
    size_t    cps_cap = 0;
    bool      ok = true;

#define PUSH_CP(cp_, idx_)                                                      \
    do {                                                                        \
        if (cps_len + 1 > cps_cap) {                                            \
            size_t new_cap = cps_cap == 0 ? 16 : cps_cap * 2;                   \
            uint32_t *grown = (uint32_t *)mdit_lib_realloc_bytes(                \
                out->lib, cps, new_cap * sizeof *cps);                          \
            if (grown == NULL) { ok = false; goto done; }                       \
            cps = grown;                                                        \
            cps_cap = new_cap;                                                  \
        }                                                                       \
        if ((idx_) < cps_len) {                                                 \
            memmove(cps + (idx_) + 1, cps + (idx_),                             \
                    (cps_len - (idx_)) * sizeof *cps);                          \
        }                                                                       \
        cps[(idx_)] = (cp_);                                                    \
        ++cps_len;                                                              \
    } while (0)

    for (size_t i = 0; i < basic_end; ++i) {
        unsigned char c = (unsigned char)input.data[i];
        if (c >= 0x80) { ok = false; goto done; }
        PUSH_CP((uint32_t)c, cps_len);
    }

    /* Decode extended part. */
    size_t   pos     = (last == (size_t)-1) ? 0 : (basic_end + 1);
    uint32_t n_value = PC_INITIAL_N;
    uint64_t i_idx   = 0;
    unsigned bias    = PC_INITIAL_BIAS;

    while (pos < input.len) {
        uint64_t old_i = i_idx;
        uint64_t w     = 1;
        unsigned k     = PC_BASE;
        for (;;) {
            if (pos >= input.len) { ok = false; goto done; }
            int digit = pc_basic_to_digit((unsigned char)input.data[pos++]);
            if (digit < 0) { ok = false; goto done; }
            if ((uint64_t)digit > (UINT64_MAX - i_idx) / w) {
                ok = false; goto done;  /* overflow */
            }
            i_idx += (uint64_t)digit * w;
            unsigned t;
            if (k <= bias)                t = PC_TMIN;
            else if (k >= bias + PC_TMAX) t = PC_TMAX;
            else                          t = k - bias;
            if ((unsigned)digit < t) break;
            if (w > UINT64_MAX / (uint64_t)(PC_BASE - t)) {
                ok = false; goto done;
            }
            w *= (uint64_t)(PC_BASE - t);
            k += PC_BASE;
        }
        bias = pc_adapt(i_idx - old_i, cps_len + 1u, old_i == 0u);

        /* n_value += i_idx / (cps_len + 1); i_idx %= cps_len + 1; */
        uint64_t outlen_p1 = (uint64_t)cps_len + 1u;
        uint64_t add = i_idx / outlen_p1;
        if ((uint64_t)n_value + add > 0x10FFFFu) { ok = false; goto done; }
        n_value += (uint32_t)add;
        i_idx   %= outlen_p1;

        PUSH_CP(n_value, (size_t)i_idx);
        ++i_idx;
    }

    /* Encode codepoints to UTF-8 into out. */
    for (size_t i = 0; i < cps_len; ++i) {
        char tmp[4];
        size_t bn = mdit_encode(cps[i], tmp, sizeof tmp);
        if (bn == 0) { ok = false; goto done; }
        if (!mdit_buf_append(out, tmp, bn)) { ok = false; goto done; }
    }

done:
    mdit_lib_free_bytes(out->lib, cps);
    return ok;
#undef PUSH_CP
}

/* ---------------------------------------------------------------------
 * IDN domain wrappers.
 *
 * Mirrors ``markdown_it._punycode``: split on
 * ``[\x2E\u3002\uFF0E\uFF61]`` (FULL STOP and three Asian ideographic
 * variants); join the per-label results with ``.``. ``@`` separates an
 * email local part which we pass through verbatim.
 * ------------------------------------------------------------------- */

/* Decode one codepoint at ``data + i``; returns its byte length. */
static size_t step_cp(const char *data, size_t len, size_t i, uint32_t *cp)
{
    return mdit_decode(data + i, len - i, cp);
}

static bool is_idn_dot(uint32_t cp)
{
    return cp == 0x002Eu || cp == 0x3002u || cp == 0xFF0Eu || cp == 0xFF61u;
}

static bool label_has_non_ascii(mdit_str label)
{
    for (size_t i = 0; i < label.len; ++i) {
        if ((unsigned char)label.data[i] >= 0x80) return true;
    }
    return false;
}

static bool label_starts_with_xn(mdit_str label)
{
    /* Strictly lowercase, matching Python's
     * ``obj.startswith("xn--")`` (which is case-sensitive). An
     * upper-cased ``XN--`` label is *not* decoded by upstream — see
     * ``markdown_it._punycode.to_unicode`` and the empirical check in
     * tests/test_punycode.c. */
    if (label.len < 4) return false;
    return label.data[0] == 'x' && label.data[1] == 'n' &&
           label.data[2] == '-' && label.data[3] == '-';
}

/* Decode one label-by-label, applying ``apply`` and joining with '.'.
 * The ``apply`` callback returns false on out-of-memory only. */
typedef bool (*pc_label_fn)(mdit_str in, mdit_buf *out);

static bool map_domain(mdit_str input, pc_label_fn apply, mdit_buf *out)
{
    /* Email split: only the part after '@' is treated as a domain. */
    size_t at = (size_t)-1;
    for (size_t i = 0; i < input.len; ++i) {
        if (input.data[i] == '@') { at = i; break; }
    }
    if (at != (size_t)-1) {
        if (!mdit_buf_append(out, input.data, at + 1u)) return false;
        input.data += at + 1u;
        input.len  -= at + 1u;
    }

    /* Split on IDN dot codepoints. */
    bool first = true;
    size_t label_start = 0;
    size_t i = 0;
    while (i < input.len) {
        uint32_t cp;
        size_t step = step_cp(input.data, input.len, i, &cp);
        if (step == 0) step = 1;
        if (is_idn_dot(cp)) {
            if (!first) {
                if (!mdit_buf_append_byte(out, '.')) return false;
            }
            first = false;
            mdit_str label = { input.data + label_start, i - label_start };
            if (!apply(label, out)) return false;
            label_start = i + step;
        }
        i += step;
    }
    if (!first) {
        if (!mdit_buf_append_byte(out, '.')) return false;
    }
    mdit_str last = { input.data + label_start, input.len - label_start };
    return apply(last, out);
}

/* ---- to_ascii: encode any non-ASCII label as "xn--<punycode>". ---- */
static bool ascii_label_apply(mdit_str label, mdit_buf *out)
{
    if (!label_has_non_ascii(label)) {
        return mdit_buf_append(out, label.data, label.len);
    }
    /* Decode the label into codepoints. */
    uint32_t *cps = NULL;
    size_t    cps_len = 0;
    size_t    cps_cap = 0;
    bool      ok = true;
    for (size_t i = 0; i < label.len; ) {
        uint32_t cp;
        size_t step = mdit_decode(label.data + i, label.len - i, &cp);
        if (step == 0) { ok = false; break; }
        if (cps_len + 1 > cps_cap) {
            size_t new_cap = cps_cap == 0 ? 16 : cps_cap * 2;
            uint32_t *grown = (uint32_t *)mdit_lib_realloc_bytes(
                out->lib, cps, new_cap * sizeof *cps);
            if (grown == NULL) { ok = false; break; }
            cps = grown;
            cps_cap = new_cap;
        }
        cps[cps_len++] = cp;
        i += step;
    }

    /* Encode into a scratch buffer; on success, prepend "xn--" and
     * append. On failure (which mirrors Python's ``suppress`` block),
     * fall back to emitting the label verbatim. */
    bool need_fallback = !ok;
    if (!need_fallback) {
        mdit_buf encoded;
        mdit_buf_init(&encoded, out->lib);
        if (!mdit_punycode_encode_cps(cps, cps_len, &encoded)) {
            need_fallback = true;
        } else {
            need_fallback = !mdit_buf_append(out, "xn--", 4);
            if (!need_fallback) {
                need_fallback = !mdit_buf_append(out, encoded.data ? encoded.data : "", encoded.len);
            }
        }
        mdit_buf_destroy(&encoded);
    }
    mdit_lib_free_bytes(out->lib, cps);
    if (need_fallback) {
        return mdit_buf_append(out, label.data, label.len);
    }
    return true;
}

/* ---- to_unicode: decode each "xn--..." label back to UTF-8. ---- */
static bool unicode_label_apply(mdit_str label, mdit_buf *out)
{
    if (!label_starts_with_xn(label)) {
        return mdit_buf_append(out, label.data, label.len);
    }
    /* Lowercase the trailing part before decoding (matches upstream's
     * ``decode(obj[4:].lower())``). The codec accepts mixed case but
     * Python normalizes for stability. */
    mdit_str trailing = { label.data + 4, label.len - 4 };
    char *tmp = NULL;
    if (trailing.len > 0) {
        tmp = (char *)mdit_lib_alloc_bytes(out->lib, trailing.len);
        if (tmp == NULL) return false;
        for (size_t i = 0; i < trailing.len; ++i) {
            unsigned char c = (unsigned char)trailing.data[i];
            if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
            tmp[i] = (char)c;
        }
        trailing.data = tmp;
    }

    mdit_buf decoded;
    mdit_buf_init(&decoded, out->lib);
    bool ok = mdit_punycode_decode_to_utf8(trailing, &decoded);
    if (ok) {
        ok = mdit_buf_append(out, decoded.data ? decoded.data : "", decoded.len);
    }
    mdit_buf_destroy(&decoded);
    mdit_lib_free_bytes(out->lib, tmp);
    if (!ok) {
        /* On failure, emit the label verbatim (suppress-equivalent). */
        return mdit_buf_append(out, label.data, label.len);
    }
    return true;
}

/* ---------------------------------------------------------------------
 * Public wrappers — copy the result into the arena.
 * ------------------------------------------------------------------- */
static mdit_str arena_copy_buf(mdit_lib_ctx *lib, mdit_arena *arena, mdit_buf b)
{
    if (b.len == 0) return MDIT_STR_LIT("");
    char *p = (char *)mdit_arena_alloc(lib, arena, b.len);
    memcpy(p, b.data, b.len);
    return (mdit_str){ p, b.len };
}

bool mdit_idn_to_ascii(mdit_lib_ctx *lib, mdit_arena *arena,
                       mdit_str hostname, mdit_str *out)
{
    mdit_buf buf;
    mdit_buf_init(&buf, lib);
    bool ok = map_domain(hostname, ascii_label_apply, &buf);
    if (ok) *out = arena_copy_buf(lib, arena, buf);
    mdit_buf_destroy(&buf);
    return ok;
}

bool mdit_idn_to_unicode(mdit_lib_ctx *lib, mdit_arena *arena,
                         mdit_str hostname, mdit_str *out)
{
    mdit_buf buf;
    mdit_buf_init(&buf, lib);
    bool ok = map_domain(hostname, unicode_label_apply, &buf);
    if (ok) *out = arena_copy_buf(lib, arena, buf);
    mdit_buf_destroy(&buf);
    return ok;
}
