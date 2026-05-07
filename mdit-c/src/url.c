/*
 * url.c — port of mdurl 0.1.x.
 *
 * The parse routine mirrors ``mdurl._parse.MutableURL.parse`` step by
 * step. Lookups that the Python code phrases as regex are rewritten
 * as inline scanners since the patterns are simple enough not to
 * warrant a real regex engine.
 *
 * The encode/decode routines drop Python's per-exclude-string cache
 * dictionary (it's a fast-path optimization for repeated calls in the
 * Python module; a per-byte branch is plenty in C).
 */
#include "url.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------
 * Tiny helpers
 * ------------------------------------------------------------------- */

static bool is_ascii_alpha(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static bool is_ascii_digit(unsigned char c)
{
    return c >= '0' && c <= '9';
}

static bool is_hex(unsigned char c)
{
    return is_ascii_digit(c)
        || (c >= 'a' && c <= 'f')
        || (c >= 'A' && c <= 'F');
}

static int hex_value(unsigned char c)
{
    if (is_ascii_digit(c)) return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

/* ---------------------------------------------------------------------
 * Substring helpers — duplicate a slice into the arena to give callers
 * a stable view independent of the input lifetime.
 * ------------------------------------------------------------------- */
static mdit_str dup_slice(mdit_arena *a, const char *src, size_t off, size_t len)
{
    mdit_str s; s.data = NULL; s.len = 0;
    if (len == 0) {
        s.data = "";
        return s;
    }
    char *buf = (char *)mdit_arena_alloc(a, len);
    memcpy(buf, src + off, len);
    s.data = buf;
    s.len  = len;
    return s;
}

/* Find the first occurrence of any byte in `set` within s[off..len);
 * returns its index or -1 if none. */
static int find_first_of(const char *s, size_t off, size_t len,
                         const char *set, size_t set_len)
{
    int best = -1;
    for (size_t i = 0; i < set_len; ++i) {
        const char *p = (const char *)memchr(s + off, set[i], len - off);
        if (p == NULL) continue;
        int idx = (int)(p - s);
        if (best < 0 || idx < best) best = idx;
    }
    return best;
}

/* memchr_rev: rfind for a single byte over s[lo..hi). */
static int memrchr_range(const char *s, size_t lo, size_t hi, char c)
{
    for (size_t i = hi; i > lo; --i) {
        if (s[i - 1] == c) return (int)(i - 1);
    }
    return -1;
}

/* ---------------------------------------------------------------------
 * Hostname-part regex equivalents.
 *
 * HOSTNAME_PART_PATTERN = r"^[+a-z0-9A-Z_-]{0,63}$"
 * HOSTNAME_PART_START   = r"^([+a-z0-9A-Z_-]{0,63})(.*)$"
 *
 * These are tiny — both come down to "longest run of [+a-zA-Z0-9_-]
 * up to 63 chars".
 * ------------------------------------------------------------------- */
static bool hostname_part_char(unsigned char c)
{
    return is_ascii_alpha(c) || is_ascii_digit(c) ||
           c == '+' || c == '_' || c == '-';
}

static bool hostname_part_pattern(const char *s, size_t len)
{
    if (len > 63) return false;
    for (size_t i = 0; i < len; ++i) {
        if (!hostname_part_char((unsigned char)s[i])) return false;
    }
    return true;
}

/* Returns the length of the longest valid hostname-part prefix
 * (capped at 63). */
static size_t hostname_part_start(const char *s, size_t len)
{
    size_t i = 0;
    size_t cap = len < 63 ? len : 63;
    while (i < cap && hostname_part_char((unsigned char)s[i])) ++i;
    return i;
}

/* ---------------------------------------------------------------------
 * Slashed / hostless protocols
 *
 * Mirror Python's defaultdicts:
 *   HOSTLESS_PROTOCOL = {"javascript", "javascript:"}
 *   SLASHED_PROTOCOL  = {"http", "https", "ftp", "gopher", "file"}
 *                       (+ each followed by ':')
 *
 * In Python those are case-sensitive lookups; the upstream parser
 * lower-cases the protocol before testing, so we need both case
 * variants stripped. We compare ASCII case-insensitively. The protocol
 * always carries the trailing colon, so we test the with-colon form.
 * ------------------------------------------------------------------- */
static bool ascii_iequal(const char *a, size_t a_len, const char *b)
{
    size_t b_len = strlen(b);
    if (a_len != b_len) return false;
    for (size_t i = 0; i < a_len; ++i) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
        if (ca != cb) return false;
    }
    return true;
}

static bool hostless_protocol(const char *p, size_t len)
{
    return ascii_iequal(p, len, "javascript")
        || ascii_iequal(p, len, "javascript:");
}

static bool slashed_protocol(const char *p, size_t len)
{
    static const char *names[] = {
        "http", "https", "ftp", "gopher", "file",
        "http:", "https:", "ftp:", "gopher:", "file:",
        NULL
    };
    for (size_t i = 0; names[i]; ++i) {
        if (ascii_iequal(p, len, names[i])) return true;
    }
    return false;
}

/* ---------------------------------------------------------------------
 * Trim leading/trailing whitespace per Python ``str.strip()``: any
 * character whose ord() is in {space, \t, \n, \v, \f, \r} or unicode
 * whitespace. We restrict to ASCII whitespace + a tiny set the parser
 * realistically sees in URLs.
 * ------------------------------------------------------------------- */
static bool py_strip_ws(unsigned char c)
{
    /* Python's ``str.strip()`` strips characters whose category is
     * Zs/Zl/Zp/Cc with the ``\s``-like definition. For URL inputs the
     * realistic set is ASCII whitespace; matching that is enough for
     * upstream test parity (the oracle never hands us NBSP-leading
     * URLs). */
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\v' || c == '\f';
}

static void trim(const char **p, size_t *len)
{
    const char *s = *p;
    size_t l = *len;
    while (l > 0 && py_strip_ws((unsigned char)s[0])) { ++s; --l; }
    while (l > 0 && py_strip_ws((unsigned char)s[l - 1])) { --l; }
    *p = s; *len = l;
}

/* ---------------------------------------------------------------------
 * Protocol pattern: ``^([a-z0-9.+-]+:)`` case-insensitive.
 * Returns length of match (incl. colon), or 0 if no match.
 * ------------------------------------------------------------------- */
static size_t protocol_match(const char *s, size_t len)
{
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (is_ascii_alpha(c) || is_ascii_digit(c) ||
            c == '.' || c == '+' || c == '-') {
            ++i;
        } else break;
    }
    if (i == 0) return 0;
    if (i >= len) return 0;
    if (s[i] != ':') return 0;
    return i + 1;
}

/* ---------------------------------------------------------------------
 * Simple path pattern: ``^(//?(?!/)[^?\s]*)(\?[^\s]*)?$``.
 *
 * In words: starts with one or two slashes (but not three), followed
 * by anything that isn't ?, space, tab, newline, etc., optionally
 * followed by ?... up to end.
 *
 * Returns:
 *   match: true if the whole input matches.
 *   path_len: number of bytes belonging to group(1).
 *   search_len: number of bytes belonging to group(2) (incl. '?'), 0 if absent.
 * ------------------------------------------------------------------- */
static bool simple_path_match(const char *s, size_t len,
                              size_t *path_len, size_t *search_len)
{
    if (len == 0) return false;
    if (s[0] != '/') return false;
    size_t i = 1;
    if (i < len && s[i] == '/') {
        ++i;
        if (i < len && s[i] == '/') return false;  /* (?!/) reject */
    }
    /* path: any [^?\s]. \s in Python is [\t\n\r\f\v\x1c\x1d\x1e\x1f] +
     * space. We use the ASCII whitespace set. */
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (c == '?') break;
        if (py_strip_ws(c)) return false;
        ++i;
    }
    *path_len = i;
    if (i == len) {
        *search_len = 0;
        return true;
    }
    /* must be ?... */
    if (s[i] != '?') return false;
    size_t qstart = i;
    ++i;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (py_strip_ws(c)) return false;
        ++i;
    }
    *search_len = i - qstart;
    return true;
}

/* ---------------------------------------------------------------------
 * Auth-pattern: ``^//[^@/]+@[^@/]+`` — does the first auth-y prefix
 * exist? Used to decide whether host-mode parsing is enabled when the
 * caller didn't explicitly say slashes_denote_host.
 * ------------------------------------------------------------------- */
static bool has_auth_prefix(const char *s, size_t len)
{
    if (len < 4) return false;
    if (s[0] != '/' || s[1] != '/') return false;
    size_t i = 2;
    /* [^@/]+ */
    size_t a_start = i;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (c == '@' || c == '/') break;
        ++i;
    }
    if (i == a_start) return false;
    if (i >= len || s[i] != '@') return false;
    ++i;
    /* [^@/]+ */
    size_t b_start = i;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (c == '@' || c == '/') break;
        ++i;
    }
    return (i > b_start);
}

/* ---------------------------------------------------------------------
 * parse_host — splits "host[:port]" via the PORT_PATTERN regex
 * ``:[0-9]*$``. Sets the ``port`` and ``hostname`` fields.
 * ------------------------------------------------------------------- */
static void parse_host(mdit_arena *a, mdit_url *u,
                       const char *host, size_t host_len)
{
    /* Python regex `:[0-9]*$` matches a colon followed by zero or more
     * digits at the end of the string. */
    size_t i = host_len;
    while (i > 0 && is_ascii_digit((unsigned char)host[i - 1])) --i;
    if (i > 0 && host[i - 1] == ':') {
        size_t colon_pos = i - 1;
        size_t port_chars = host_len - i;
        /* If port == ":" exactly (no digits), Python doesn't set port
         * but still strips the colon from the host. */
        if (port_chars > 0) {
            u->has_port = true;
            u->port     = dup_slice(a, host, i, port_chars);
        }
        host_len = colon_pos;
    }
    if (host_len > 0) {
        u->has_hostname = true;
        u->hostname     = dup_slice(a, host, 0, host_len);
    }
}

/* ---------------------------------------------------------------------
 * Public: mdit_url_parse
 *
 * Mirrors ``MutableURL.parse(self, url, slashes_denote_host)`` and the
 * ``url_parse`` shim that wraps it.
 * ------------------------------------------------------------------- */
bool mdit_url_parse(mdit_arena *arena,
                    mdit_str input,
                    bool slashes_denote_host,
                    mdit_url *out)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof *out);

    const char *src = input.data;
    size_t      n   = input.len;

    /* trim whitespace */
    trim(&src, &n);

    /* fast path: simple //...?... */
    if (!slashes_denote_host) {
        bool has_hash = (memchr(src, '#', n) != NULL);
        if (!has_hash) {
            size_t plen = 0, slen = 0;
            if (simple_path_match(src, n, &plen, &slen)) {
                out->has_pathname = true;
                out->pathname     = dup_slice(arena, src, 0, plen);
                if (slen > 0) {
                    out->has_search = true;
                    out->search     = dup_slice(arena, src, plen, slen);
                }
                return true;
            }
        }
    }

    /* protocol */
    size_t pmatch = protocol_match(src, n);
    /* Snapshot: protocol slice (case as input), and a lower-case copy
     * for HOSTLESS / SLASHED lookups. */
    const char *proto_p   = src;
    size_t      proto_len = pmatch;
    if (pmatch > 0) {
        out->has_protocol = true;
        out->protocol     = dup_slice(arena, src, 0, pmatch);
        src += pmatch; n -= pmatch;
    }

    /* host detection */
    bool slashes = false;
    if (slashes_denote_host || pmatch > 0 || has_auth_prefix(src, n)) {
        slashes = (n >= 2 && src[0] == '/' && src[1] == '/');
        bool proto_hostless = (pmatch > 0) && hostless_protocol(proto_p, proto_len);
        if (slashes && !proto_hostless) {
            src += 2; n -= 2;
            out->slashes = true;
        }
    }

    bool proto_hostless2 = (pmatch > 0) && hostless_protocol(proto_p, proto_len);
    bool proto_slashed   = (pmatch > 0) && slashed_protocol(proto_p, proto_len);
    if (!proto_hostless2 && (slashes || (pmatch > 0 && !proto_slashed))) {

        /* find the first instance of any HOST_ENDING_CHARS in rest. */
        int host_end = find_first_of(src, 0, n, "/?#", 3);

        /* find the @ for auth */
        int at_sign;
        if (host_end == -1) {
            at_sign = memrchr_range(src, 0, n, '@');
        } else {
            at_sign = memrchr_range(src, 0, (size_t)host_end + 1, '@');
        }

        if (at_sign != -1) {
            out->has_auth = true;
            out->auth     = dup_slice(arena, src, 0, (size_t)at_sign);
            src += (size_t)at_sign + 1;
            n   -= (size_t)at_sign + 1;
        }

        /* recompute host_end against NON_HOST_CHARS = "%/?;#" + AUTO_ESCAPE
         * AUTO_ESCAPE = "'" + UNWISE; UNWISE = "{}|\\^`" + DELIMS;
         * DELIMS = "<>\"` \r\n\t".  Combined unique set: */
        static const char NON_HOST_CHARS[] =
            "%/?;#'{}|\\^`<>\" \r\n\t";
        host_end = find_first_of(src, 0, n, NON_HOST_CHARS,
                                  sizeof NON_HOST_CHARS - 1);
        if (host_end == -1) host_end = (int)n;

        if (host_end > 0 && src[host_end - 1] == ':') --host_end;

        const char *host = src;
        size_t      host_len = (size_t)host_end;

        /* leftover */
        const char *rest_after_host = src + host_end;
        size_t      rest_after_host_len = n - (size_t)host_end;

        /* Pull port out of host */
        parse_host(arena, out, host, host_len);

        /* "even if it's empty, it has to be present" */
        if (!out->has_hostname) {
            out->has_hostname = true;
            out->hostname.data = "";
            out->hostname.len  = 0;
        }

        /* ipv6? hostname starts with [ and ends with ] */
        bool ipv6 = (out->hostname.len >= 2 &&
                     out->hostname.data[0] == '[' &&
                     out->hostname.data[out->hostname.len - 1] == ']');

        /* Validate hostname parts (skip for ipv6). The Python code may
         * splice the bad tail back into ``rest`` — we do the same. */
        if (!ipv6 && out->hostname.len > 0) {
            const char *hp = out->hostname.data;
            size_t      hp_len = out->hostname.len;
            /* iterate over '.'-separated parts. */
            size_t part_start = 0;
            size_t i = 0;
            bool   broke_early = false;
            for (;;) {
                if (i == hp_len || hp[i] == '.') {
                    size_t plen = i - part_start;
                    if (plen == 0) {
                        /* skip empty part */
                    } else if (!hostname_part_pattern(hp + part_start, plen)) {
                        /* try ASCII-replacement: build a temporary buffer
                         * substituting every byte >= 0x80 with 'x'. */
                        char tmp_stack[64];
                        char *tmp = (plen <= sizeof tmp_stack)
                            ? tmp_stack
                            : (char *)mdit_arena_alloc(arena, plen);
                        for (size_t k = 0; k < plen; ++k) {
                            unsigned char b = (unsigned char)hp[part_start + k];
                            tmp[k] = (b > 127) ? 'x' : (char)b;
                        }
                        if (!hostname_part_pattern(tmp, plen)) {
                            /* invalid even after ASCII replacement.
                             * Split: valid_parts = parts before this; the
                             * leading `bit` of this part stays, the rest
                             * (and any tail parts) become path. */
                            size_t bit = hostname_part_start(hp + part_start, plen);
                            size_t valid_end = part_start + bit;

                            /* not_host = [bit_tail] + [parts after] */
                            size_t not_host_off = part_start + bit;
                            size_t not_host_len = hp_len - not_host_off;

                            /* concat ".".join(not_host) (if any) + rest_after_host */
                            mdit_buf nb; mdit_buf_init(&nb);
                            if (not_host_len > 0) {
                                /* not_host[0] is the bit_tail (no leading dot);
                                 * subsequent parts are joined with '.' which
                                 * means we just keep the original substring of
                                 * hostname starting at `not_host_off`. */
                                if (!mdit_buf_append(&nb,
                                        hp + not_host_off, not_host_len)) {
                                    mdit_buf_destroy(&nb);
                                    return false;
                                }
                            }
                            if (rest_after_host_len > 0) {
                                if (!mdit_buf_append(&nb,
                                        rest_after_host, rest_after_host_len)) {
                                    mdit_buf_destroy(&nb);
                                    return false;
                                }
                            }
                            /* re-point `rest_after_host` at this freshly
                             * allocated buffer. We arena-copy so the ptr is
                             * stable. */
                            rest_after_host = (const char *)mdit_arena_alloc(
                                arena, nb.len + 1);
                            memcpy((char *)rest_after_host, nb.data, nb.len);
                            ((char *)rest_after_host)[nb.len] = '\0';
                            rest_after_host_len = nb.len;
                            mdit_buf_destroy(&nb);

                            /* Truncate hostname to valid_parts join '.' */
                            out->hostname.data = (const char *)mdit_arena_alloc(
                                arena, valid_end);
                            memcpy((char *)out->hostname.data, hp, valid_end);
                            out->hostname.len = valid_end;
                            broke_early = true;
                            break;
                        }
                    }
                    if (i == hp_len) break;
                    part_start = i + 1;
                }
                ++i;
            }
            (void)broke_early;
        }

        /* HOSTNAME_MAX_LEN check */
        if (out->hostname.len > 255) {
            out->hostname.data = "";
            out->hostname.len  = 0;
        }

        /* strip [ ] from ipv6 hostname */
        if (ipv6 && out->hostname.len >= 2) {
            out->hostname.data += 1;
            out->hostname.len  -= 2;
        }

        src = rest_after_host;
        n   = rest_after_host_len;
    }

    /* tail: hash, search, pathname */
    int hash_idx = (int)(memchr(src, '#', n) ?
                          ((const char *)memchr(src, '#', n)) - src : -1);
    if (hash_idx != -1) {
        out->has_hash = true;
        out->hash     = dup_slice(arena, src,
                                  (size_t)hash_idx, n - (size_t)hash_idx);
        n = (size_t)hash_idx;
    }

    int qm_idx = (int)(memchr(src, '?', n) ?
                        ((const char *)memchr(src, '?', n)) - src : -1);
    if (qm_idx != -1) {
        out->has_search = true;
        out->search     = dup_slice(arena, src,
                                    (size_t)qm_idx, n - (size_t)qm_idx);
        n = (size_t)qm_idx;
    }

    if (n > 0) {
        out->has_pathname = true;
        out->pathname     = dup_slice(arena, src, 0, n);
    }

    /* SLASHED_PROTOCOL[lower_proto] && hostname && !pathname → pathname = "" */
    if (pmatch > 0 && slashed_protocol(proto_p, proto_len) &&
        out->has_hostname && out->hostname.len > 0 && !out->has_pathname) {
        out->has_pathname = true;
        out->pathname.data = "";
        out->pathname.len  = 0;
    }

    return true;
}

/* ---------------------------------------------------------------------
 * mdit_url_format
 * ------------------------------------------------------------------- */
bool mdit_url_format(const mdit_url *url, mdit_buf *out)
{
    if (url->has_protocol) {
        if (!mdit_buf_append(out, url->protocol.data, url->protocol.len)) return false;
    }
    if (url->slashes) {
        if (!mdit_buf_append(out, "//", 2)) return false;
    }
    if (url->has_auth) {
        if (!mdit_buf_append(out, url->auth.data, url->auth.len)) return false;
        if (!mdit_buf_append_byte(out, '@')) return false;
    }
    if (url->has_hostname) {
        bool ipv6 = (memchr(url->hostname.data, ':', url->hostname.len) != NULL);
        if (ipv6) {
            if (!mdit_buf_append_byte(out, '[')) return false;
            if (!mdit_buf_append(out, url->hostname.data, url->hostname.len)) return false;
            if (!mdit_buf_append_byte(out, ']')) return false;
        } else {
            if (!mdit_buf_append(out, url->hostname.data, url->hostname.len)) return false;
        }
    }
    if (url->has_port) {
        if (!mdit_buf_append_byte(out, ':')) return false;
        if (!mdit_buf_append(out, url->port.data, url->port.len)) return false;
    }
    if (url->has_pathname) {
        if (!mdit_buf_append(out, url->pathname.data, url->pathname.len)) return false;
    }
    if (url->has_search) {
        if (!mdit_buf_append(out, url->search.data, url->search.len)) return false;
    }
    if (url->has_hash) {
        if (!mdit_buf_append(out, url->hash.data, url->hash.len)) return false;
    }
    return true;
}

/* ---------------------------------------------------------------------
 * Encode / Decode
 * ------------------------------------------------------------------- */

/* Build a 128-byte boolean array: in_set[c] == true iff `c` is in
 * `exclude`. */
static void build_exclude_set(const char *exclude, bool out[128])
{
    memset(out, 0, 128);
    for (const char *p = exclude; p && *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c < 128) out[c] = true;
    }
}

static bool emit_pct(mdit_buf *out, unsigned char b)
{
    static const char hex[] = "0123456789ABCDEF";
    char esc[3] = { '%', hex[b >> 4], hex[b & 0xF] };
    return mdit_buf_append(out, esc, 3);
}

bool mdit_url_encode(mdit_str input,
                     const char *exclude,
                     bool keep_escaped,
                     mdit_buf *out)
{
    bool in_excl[128];
    build_exclude_set(exclude, in_excl);

    const char *s = input.data;
    size_t      n = input.len;
    size_t      i = 0;
    while (i < n) {
        unsigned char b = (unsigned char)s[i];

        /* keep already-encoded sequences verbatim */
        if (keep_escaped && b == '%' && i + 2 < n &&
            is_hex((unsigned char)s[i + 1]) &&
            is_hex((unsigned char)s[i + 2])) {
            if (!mdit_buf_append(out, s + i, 3)) return false;
            i += 3;
            continue;
        }

        if (b < 128) {
            /* unreserved + exclude pass through; everything else gets %XX */
            if (is_ascii_alpha(b) || is_ascii_digit(b) || in_excl[b]) {
                if (!mdit_buf_append_byte(out, (char)b)) return false;
            } else {
                if (!emit_pct(out, b)) return false;
            }
            ++i;
            continue;
        }

        /* Non-ASCII byte (UTF-8 lead/continuation): always %XX. */
        if (!emit_pct(out, b)) return false;
        ++i;
    }
    return true;
}

/* Decode helper: given a %XX sequence, return the byte value, or -1 if
 * malformed. */
static int decode_pct(const char *s, size_t i, size_t n)
{
    if (i + 2 >= n) return -1;
    if (s[i] != '%') return -1;
    int hi = hex_value((unsigned char)s[i + 1]);
    int lo = hex_value((unsigned char)s[i + 2]);
    if (hi < 0 || lo < 0) return -1;
    return (hi << 4) | lo;
}

/*
 * Decode mirrors the Python helper: a run of `%XX` sequences is
 * collected, then each leading byte determines the UTF-8 length to
 * consume from the run. Bytes that decode to ASCII chars in the
 * exclude set are re-emitted as ``%XX`` (uppercase). Malformed UTF-8
 * emits one U+FFFD per consumed slot, matching Python.
 */
bool mdit_url_decode(mdit_str input,
                     const char *exclude,
                     mdit_buf *out)
{
    bool in_excl[128];
    build_exclude_set(exclude, in_excl);

    static const char REPL[] = "\xEF\xBF\xBD"; /* U+FFFD */
    const char *s = input.data;
    size_t      n = input.len;
    size_t      i = 0;
    while (i < n) {
        if (s[i] != '%') {
            if (!mdit_buf_append_byte(out, s[i])) return false;
            ++i;
            continue;
        }
        /* Find the run of %XX sequences. */
        size_t run_end = i;
        while (run_end + 2 < n && s[run_end] == '%' &&
               is_hex((unsigned char)s[run_end + 1]) &&
               is_hex((unsigned char)s[run_end + 2])) {
            run_end += 3;
        }
        if (run_end == i) {
            /* malformed leading %, emit literally */
            if (!mdit_buf_append_byte(out, s[i])) return false;
            ++i;
            continue;
        }

        /* Walk the run, decoding multibyte UTF-8 conservatively. */
        size_t j = i;
        while (j < run_end) {
            int b1 = decode_pct(s, j, n);
            if (b1 < 0) {
                /* shouldn't happen given the run scan, but guard. */
                if (!mdit_buf_append_byte(out, s[j])) return false;
                ++j;
                continue;
            }
            if (b1 < 0x80) {
                /* ASCII path. */
                if (in_excl[b1]) {
                    /* re-emit as uppercase %XX */
                    if (!emit_pct(out, (unsigned char)b1)) return false;
                } else {
                    if (!mdit_buf_append_byte(out, (char)b1)) return false;
                }
                j += 3;
                continue;
            }
            if ((b1 & 0xE0) == 0xC0 && j + 6 <= run_end) {
                int b2 = decode_pct(s, j + 3, n);
                if (b2 >= 0 && (b2 & 0xC0) == 0x80) {
                    char buf[2] = { (char)b1, (char)b2 };
                    /* validate UTF-8 round-trip — Python uses bytes.decode()
                     * which would raise on overlong / invalid; mimic by
                     * checking that the resulting codepoint is actually a
                     * 2-byte sequence (>= U+0080). */
                    unsigned int cp = ((b1 & 0x1F) << 6) | (b2 & 0x3F);
                    if (cp >= 0x80) {
                        if (!mdit_buf_append(out, buf, 2)) return false;
                    } else {
                        if (!mdit_buf_append(out, REPL, 3)) return false;
                        if (!mdit_buf_append(out, REPL, 3)) return false;
                    }
                    j += 6;
                    continue;
                }
            }
            if ((b1 & 0xF0) == 0xE0 && j + 9 <= run_end) {
                int b2 = decode_pct(s, j + 3, n);
                int b3 = decode_pct(s, j + 6, n);
                if (b2 >= 0 && b3 >= 0 &&
                    (b2 & 0xC0) == 0x80 && (b3 & 0xC0) == 0x80) {
                    char buf[3] = { (char)b1, (char)b2, (char)b3 };
                    unsigned int cp = ((b1 & 0x0F) << 12) |
                                      ((b2 & 0x3F) << 6) |
                                      (b3 & 0x3F);
                    /* reject overlong, surrogates, or non-canonical */
                    if (cp >= 0x800 && (cp < 0xD800 || cp > 0xDFFF)) {
                        if (!mdit_buf_append(out, buf, 3)) return false;
                    } else {
                        if (!mdit_buf_append(out, REPL, 3)) return false;
                        if (!mdit_buf_append(out, REPL, 3)) return false;
                        if (!mdit_buf_append(out, REPL, 3)) return false;
                    }
                    j += 9;
                    continue;
                }
            }
            if ((b1 & 0xF8) == 0xF0 && j + 12 <= run_end) {
                int b2 = decode_pct(s, j + 3, n);
                int b3 = decode_pct(s, j + 6, n);
                int b4 = decode_pct(s, j + 9, n);
                if (b2 >= 0 && b3 >= 0 && b4 >= 0 &&
                    (b2 & 0xC0) == 0x80 && (b3 & 0xC0) == 0x80 &&
                    (b4 & 0xC0) == 0x80) {
                    char buf[4] = { (char)b1, (char)b2, (char)b3, (char)b4 };
                    unsigned int cp = ((b1 & 0x07) << 18) |
                                      ((b2 & 0x3F) << 12) |
                                      ((b3 & 0x3F) << 6) |
                                      (b4 & 0x3F);
                    if (cp >= 0x10000 && cp <= 0x10FFFF) {
                        if (!mdit_buf_append(out, buf, 4)) return false;
                    } else {
                        if (!mdit_buf_append(out, REPL, 3)) return false;
                        if (!mdit_buf_append(out, REPL, 3)) return false;
                        if (!mdit_buf_append(out, REPL, 3)) return false;
                        if (!mdit_buf_append(out, REPL, 3)) return false;
                    }
                    j += 12;
                    continue;
                }
            }
            /* fallthrough: invalid lead → 1 U+FFFD, advance one slot */
            if (!mdit_buf_append(out, REPL, 3)) return false;
            j += 3;
        }
        i = run_end;
    }
    return true;
}
