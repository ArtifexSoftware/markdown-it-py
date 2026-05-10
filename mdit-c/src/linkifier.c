/*
 * linkifier.c — default linkifier implementation.
 *
 * Reproduces the URL-boundary heuristics from linkify-it-py for the
 * subset listed in the header. The hot path is hand-written; we avoid
 * a regex engine entirely.
 *
 * The algorithm is roughly:
 *
 *   1. `pretest(text)` looks for a hint character (`:`, `@`, `.`).
 *   2. For each hint, attempt to recognise:
 *        - explicit `http://` / `https://` / `ftp://` URL
 *        - `mailto:` URL
 *        - `//host/...` protocol-relative URL
 *        - bare email `local@host.tld`
 *        - bare URL `host.tld/...` / `www.host.tld`
 *   3. URL boundary: walk forward consuming "URL bytes"; trailing
 *      punctuation is trimmed unless it sits inside balanced
 *      brackets/parens/quotes (for path components).
 *
 * `match_at_start` only returns a match if it starts exactly at byte
 * 0, mirroring upstream's `match_at_start`.
 */
#include "linkifier.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

#include "arena.h"
#include "linkifier_tlds_full.h"
#include "utf.h"

/* Process-wide toggle for the full ICANN TLD list. Mirrors upstream
 * `Linkify.tlds(TLDS, keep_old=True)`. The flag is non-atomic on
 * purpose: upstream's API is also not thread-safe and callers are
 * expected to set it during process init. */
static bool g_use_full_tlds = false;

/* Forward declarations for the trivial ASCII char-class helpers
 * defined later (they're tiny but referenced from `is_xn_punycode`
 * which logically belongs in the TLD section). */
static bool is_ascii_alpha(unsigned char c);
static bool is_ascii_digit(unsigned char c);
static bool is_ascii_alnum(unsigned char c);

/* ---------------------------------------------------------------------
 * TLD recognition
 *
 * Default TLD set (matches upstream `Linkify()` defaults):
 *
 *   - the small built-in list:
 *       biz com edu gov net org pro web xxx aero asia coop info
 *       museum name shop рф
 *   - all 2-character ASCII ccTLDs (a[cdefgilmnoqrstuwxz] | b[abdefghijmnorstvwyz] | …)
 *   - Punycode labels `xn--<1..59 ASCII alnum/hyphens>` (IDN)
 *
 * Encoded as a flat sorted array for binary search. Comparisons are
 * ASCII case-insensitive; the only non-ASCII default TLD is "рф"
 * (Cyrillic), matched byte-for-byte against the UTF-8 form
 * `\xd1\x80\xd1\x84`.
 *
 * If `g_use_full_tlds` is true, the full ICANN list (see
 * `linkifier_tlds_full.h`, ~1500 entries) is also consulted. This
 * matches upstream's `Linkify.tlds(TLDS, keep_old=True)`.
 * ------------------------------------------------------------------- */

/* Sorted (case-insensitive ASCII), unique. Two-char ccTLDs derive from
 * upstream's tlds_2ch_src_re. The Cyrillic "рф" is handled separately.
 *
 * Generated from linkify-it-py 2.1.0 default list. */
static const char *const TLDS[] = {
    "ac", "ad", "ae", "aero", "af", "ag", "ai", "al", "am", "ao",
    "aq", "ar", "as", "asia", "at", "au", "aw", "ax", "az",
    "ba", "bb", "bd", "be", "bf", "bg", "bh", "bi", "biz", "bj",
    "bm", "bn", "bo", "br", "bs", "bt", "bv", "bw", "by", "bz",
    "ca", "cc", "cd", "cf", "cg", "ch", "ci", "ck", "cl", "cm",
    "cn", "co", "com", "coop", "cr", "cu", "cv", "cw", "cx", "cy",
    "cz",
    "de", "dj", "dk", "dm", "do", "dz",
    "ec", "edu", "ee", "eg", "er", "es", "et", "eu",
    "fi", "fj", "fk", "fm", "fo", "fr",
    "ga", "gb", "gd", "ge", "gf", "gg", "gh", "gi", "gl", "gm",
    "gn", "gov", "gp", "gq", "gr", "gs", "gt", "gu", "gw", "gy",
    "hk", "hm", "hn", "hr", "ht", "hu",
    "id", "ie", "il", "im", "in", "info", "io", "iq", "ir", "is",
    "it",
    "je", "jm", "jo", "jp",
    "ke", "kg", "kh", "ki", "km", "kn", "kp", "kr", "kw", "ky",
    "kz",
    "la", "lb", "lc", "li", "lk", "lr", "ls", "lt", "lu", "lv",
    "ly",
    "ma", "mc", "md", "me", "mg", "mh", "mk", "ml", "mm", "mn",
    "mo", "mp", "mq", "mr", "ms", "mt", "mu", "museum", "mv", "mw",
    "mx", "my", "mz",
    "na", "name", "nc", "ne", "net", "nf", "ng", "ni", "nl", "no",
    "np", "nr", "nu", "nz",
    "om", "org",
    "pa", "pe", "pf", "pg", "ph", "pk", "pl", "pm", "pn", "pr",
    "pro", "ps", "pt", "pw", "py",
    "qa",
    "re", "ro", "rs", "ru", "rw",
    "sa", "sb", "sc", "sd", "se", "sg", "sh", "shop", "si", "sj",
    "sk", "sl", "sm", "sn", "so", "sr", "st", "su", "sv", "sx",
    "sy", "sz",
    "tc", "td", "tf", "tg", "th", "tj", "tk", "tl", "tm", "tn",
    "to", "tr", "tt", "tv", "tw", "tz",
    "ua", "ug", "uk", "us", "uy", "uz",
    "va", "vc", "ve", "vg", "vi", "vn", "vu",
    "web", "wf", "ws",
    "xxx",
    "ye", "yt",
    "za", "zm", "zw"
};
#define N_TLDS (sizeof(TLDS) / sizeof(TLDS[0]))

/* The Cyrillic "рф" (U+0440 U+0444) — UTF-8 is 4 bytes. */
static const char TLD_CYR_RF[] = "\xd1\x80\xd1\x84";

static int ascii_icmp(const char *a, size_t alen,
                      const char *b, size_t blen)
{
    size_t n = (alen < blen) ? alen : blen;
    for (size_t i = 0; i < n; ++i) {
        int ca = tolower((unsigned char)a[i]);
        int cb = tolower((unsigned char)b[i]);
        if (ca != cb) return ca - cb;
    }
    if (alen < blen) return -1;
    if (alen > blen) return 1;
    return 0;
}

/* IDN Punycode prefix: any label of the form `xn--<1..59 ASCII alnum/-/_>`.
 * The trailing-length cap mirrors the DNS 63-octet label limit. The
 * first character after `xn--` must be alnum (no leading hyphen) to
 * match upstream's `[a-z0-9]+(-[a-z0-9]+)*` shape. */
static bool is_xn_punycode(const char *s, size_t n)
{
    if (n < 5 || n > 63) return false;
    if (!(s[0] == 'x' || s[0] == 'X')) return false;
    if (!(s[1] == 'n' || s[1] == 'N')) return false;
    if (s[2] != '-' || s[3] != '-') return false;
    /* At least one alnum after the `xn--`. */
    if (!is_ascii_alnum((unsigned char)s[4])) return false;
    for (size_t i = 5; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (is_ascii_alnum(c) || c == '-') continue;
        return false;
    }
    return true;
}

static bool tld_lookup(const char *s, size_t n)
{
    if (n == sizeof(TLD_CYR_RF) - 1 &&
        memcmp(s, TLD_CYR_RF, n) == 0) return true;
    /* Binary search the ASCII default list. */
    size_t lo = 0, hi = N_TLDS;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const char *t = TLDS[mid];
        int c = ascii_icmp(s, n, t, strlen(t));
        if (c == 0) return true;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    /* Punycode IDN labels. Recognised regardless of the full-list
     * toggle — upstream includes `xn--*` in its default `src_xn`. */
    if (is_xn_punycode(s, n)) return true;
    /* Optional: full ICANN TLD list. */
    if (g_use_full_tlds && mdit_linkifier_full_tld_lookup(s, n)) {
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------------
 * Character classes (subset of `_re_src_path`)
 *
 * `pseudo_letter`: anything that's not whitespace / control / Unicode
 *                  punctuation+symbol (we approximate via ASCII-only
 *                  rules + `mdit_is_punct` for non-ASCII).
 * ------------------------------------------------------------------- */
static bool is_ascii_alpha(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static bool is_ascii_digit(unsigned char c) { return c >= '0' && c <= '9'; }

static bool is_ascii_alnum(unsigned char c)
{
    return is_ascii_alpha(c) || is_ascii_digit(c);
}

/* Pseudo-letter for hostname labels: alphanumeric or non-ASCII printable. */
static bool is_pseudo_letter_byte(unsigned char c)
{
    if (is_ascii_alnum(c)) return true;
    if (c >= 0x80) return true;  /* non-ASCII; trust UTF-8 decode below */
    return false;
}

/* "URL byte" allowed in path component: anything that's not whitespace,
 * control, or one of our manually-handled stop chars. We handle
 * trailing-punctuation trimming as a separate pass. */
static bool is_url_byte(unsigned char c)
{
    if (c <= 0x20) return false;     /* whitespace + control */
    if (c == 0x7F) return false;
    if (c == '<' || c == '>') return false;
    return true;
}

/* ---------------------------------------------------------------------
 * Hostname scanner.
 *
 * Mirrors `SRC_HOST` (label . label . label) where each label is a
 * pseudo-letter sequence, optionally with internal hyphens. The total
 * scan length is returned, or 0 on failure.
 *
 * `out_tld_off` / `out_tld_len` record the byte range of the final
 * label (for fuzzy TLD validation).
 * ------------------------------------------------------------------- */
/* IDN Punycode label `xn--<1..59 ASCII alnum/->`. Returns 0 if not an
 * `xn--` label, else the byte length of the label.
 *
 * Mirrors upstream's `src_xn = xn--[a-z0-9\-]{1,59}`, which (unlike
 * the plain pseudo-letter label) allows consecutive hyphens. */
static size_t scan_xn_label(const char *s, size_t n)
{
    if (n < 5) return 0;
    if (!(s[0] == 'x' || s[0] == 'X')) return 0;
    if (!(s[1] == 'n' || s[1] == 'N')) return 0;
    if (s[2] != '-' || s[3] != '-') return 0;
    /* `[a-z0-9\-]{1,59}` after the prefix. */
    size_t i = 4;
    size_t cap = (n < 4 + 59) ? n : (4 + 59);
    while (i < cap) {
        unsigned char c = (unsigned char)s[i];
        if (is_ascii_alnum(c) || c == '-') { ++i; continue; }
        break;
    }
    if (i == 4) return 0;
    return i;
}

static size_t scan_host_label(const char *s, size_t n)
{
    /* Punycode IDN label first: matches upstream's `src_xn` alt. */
    size_t xn = scan_xn_label(s, n);
    if (xn > 0) return xn;

    if (n == 0 || !is_pseudo_letter_byte((unsigned char)s[0])) return 0;
    size_t i = 1;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (is_pseudo_letter_byte(c)) { ++i; continue; }
        if (c == '-' && i + 1 < n &&
            is_pseudo_letter_byte((unsigned char)s[i + 1])) {
            ++i; continue;
        }
        break;
    }
    return i;
}

static size_t scan_host(const char *s, size_t n,
                        size_t *out_tld_off, size_t *out_tld_len)
{
    size_t i = 0;
    size_t last_label_off = 0, last_label_len = 0;
    size_t last_consumed = 0;
    for (;;) {
        size_t lbl = scan_host_label(s + i, n - i);
        if (lbl == 0) break;
        last_label_off = i;
        last_label_len = lbl;
        i += lbl;
        last_consumed = i;
        /* If next is `.` followed by another label, keep going.
         * A trailing `.` (no label after) is left for the surrounding
         * text to consume — mirrors upstream's smart trailing-punctuation
         * trim. */
        if (i + 1 < n && s[i] == '.' &&
            (is_pseudo_letter_byte((unsigned char)s[i + 1]))) {
            ++i;
            continue;
        }
        break;
    }
    if (last_consumed == 0) return 0;
    if (out_tld_off) *out_tld_off = last_label_off;
    if (out_tld_len) *out_tld_len = last_label_len;
    return last_consumed;
}

/* Optional `:port` after host. */
static size_t scan_port(const char *s, size_t n)
{
    if (n == 0 || s[0] != ':') return 0;
    size_t i = 1;
    while (i < n && is_ascii_digit((unsigned char)s[i])) ++i;
    if (i == 1) return 0;
    return i;
}

/* Path scanner: start with `/?#`, consume URL bytes with smart
 * trailing-punctuation handling. Mirrors `_re_src_path`. */
static size_t scan_path(const char *s, size_t n)
{
    if (n == 0) return 0;
    char first = s[0];
    if (first != '/' && first != '?' && first != '#') return 0;

    /* Bracket nesting counters for balanced ()[]{}'" inside the path. */
    int paren = 0, bracket = 0, brace = 0;
    size_t i = 0;

    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (!is_url_byte(c)) break;

        if (c == '(') { ++paren; ++i; continue; }
        if (c == ')') {
            if (paren == 0) break;
            --paren; ++i; continue;
        }
        if (c == '[') { ++bracket; ++i; continue; }
        if (c == ']') {
            if (bracket == 0) break;
            --bracket; ++i; continue;
        }
        if (c == '{') { ++brace; ++i; continue; }
        if (c == '}') {
            if (brace == 0) break;
            --brace; ++i; continue;
        }
        ++i;
    }

    /* Trim trailing punctuation. Don't trim if it would leave us
     * shorter than the minimum (just the leading `/?#`). */
    while (i > 1) {
        unsigned char c = (unsigned char)s[i - 1];
        if (c == '.' || c == ',' || c == ';' || c == ':' ||
            c == '!' || c == '?' || c == '*' || c == '\'' ||
            c == '"' || c == '-') {
            --i;
            continue;
        }
        break;
    }

    /* "/" alone (just the slash) is a valid path. */
    return i;
}

/* ---------------------------------------------------------------------
 * Schema scanner. Matches `[a-zA-Z][a-zA-Z0-9+.-]*` and returns the
 * length, including the trailing `:`. Returns 0 on no match.
 * ------------------------------------------------------------------- */
static size_t scan_scheme(const char *s, size_t n)
{
    if (n == 0 || !is_ascii_alpha((unsigned char)s[0])) return 0;
    size_t i = 1;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (is_ascii_alnum(c) || c == '+' || c == '.' || c == '-') {
            ++i;
            continue;
        }
        break;
    }
    if (i >= n || s[i] != ':') return 0;
    return i;  /* without the colon */
}

/* Recognise one of "http", "https", "ftp" (case-insensitive). */
static bool is_http_scheme(const char *s, size_t n)
{
    if (n == 4 && ascii_icmp(s, 4, "http", 4) == 0) return true;
    if (n == 5 && ascii_icmp(s, 5, "https", 5) == 0) return true;
    if (n == 3 && ascii_icmp(s, 3, "ftp", 3) == 0) return true;
    return false;
}

static bool is_mailto_scheme(const char *s, size_t n)
{
    return n == 6 && ascii_icmp(s, 6, "mailto", 6) == 0;
}

/* ---------------------------------------------------------------------
 * Auth (`user[:pass]@`) scanner. Used by `http://user:pass@host/...`.
 * Returns the byte length consumed (including the trailing `@`), or 0.
 * ------------------------------------------------------------------- */
static size_t scan_auth(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c <= 0x20 || c == 0x7F) return 0;
        if (c == '@') return i + 1;
        if (c == '/' || c == '[' || c == ']' || c == '(' || c == ')') {
            return 0;
        }
        ++i;
    }
    return 0;
}

/* ---------------------------------------------------------------------
 * Email-local-part scanner. Mirrors `SRC_EMAIL_NAME`:
 *   [-:&=+$,.a-zA-Z0-9_][-:&=+$,".a-zA-Z0-9_]*
 * Returns length consumed (>=1), or 0.
 * ------------------------------------------------------------------- */
static bool is_email_local_first(unsigned char c)
{
    if (is_ascii_alnum(c)) return true;
    switch (c) {
        case '-': case ':': case '&': case '=': case '+': case '$':
        case ',': case '.': case '_':
            return true;
        default: return false;
    }
}

static bool is_email_local_rest(unsigned char c)
{
    if (is_email_local_first(c)) return true;
    return c == '"';
}

static size_t scan_email_local(const char *s, size_t n)
{
    if (n == 0 || !is_email_local_first((unsigned char)s[0])) return 0;
    size_t i = 1;
    while (i < n && is_email_local_rest((unsigned char)s[i])) ++i;
    return i;
}

/* ---------------------------------------------------------------------
 * Pre-flight / hint test.
 * ------------------------------------------------------------------- */
static bool df_pretest(void *self, mdit_lib_ctx *lib, mdit_str text)
{
    (void)self;
    (void)lib;
    /* True if the text contains `://`, `@`, or a `.` followed by a
     * letter/digit (cheap proxy for fuzzy host). */
    for (size_t i = 0; i + 2 < text.len; ++i) {
        if (text.data[i] == ':' && text.data[i + 1] == '/' &&
            text.data[i + 2] == '/') return true;
    }
    for (size_t i = 0; i < text.len; ++i) {
        if (text.data[i] == '@') return true;
    }
    for (size_t i = 0; i + 1 < text.len; ++i) {
        if (text.data[i] == '.' &&
            is_pseudo_letter_byte((unsigned char)text.data[i + 1])) {
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------------
 * Core matchers.
 *
 * Each tries to recognise a URL/email starting at byte `pos` in
 * `text`, writing the result into `out` on success. Returns the
 * "schema length" actually consumed before the host (for the fuzzy
 * vs strict distinction).
 * ------------------------------------------------------------------- */
static bool emit_match(mdit_lib_ctx *lib, mdit_arena *arena,
                       mdit_str schema_lit,
                       mdit_str text,
                       size_t start, size_t end,
                       const char *url_prefix,
                       size_t url_prefix_len,
                       mdit_linkify_match *out)
{
    size_t raw_len = end - start;
    out->schema = schema_lit;
    out->index = start;
    out->last_index = end;
    out->text = (mdit_str){ text.data + start, raw_len };

    /* If a prefix is provided, emit url = prefix + raw text. */
    if (url_prefix_len > 0) {
        char *buf = (char *)mdit_arena_alloc(lib, arena,
                                             url_prefix_len + raw_len);
        if (buf == NULL) return false;
        memcpy(buf, url_prefix, url_prefix_len);
        memcpy(buf + url_prefix_len, text.data + start, raw_len);
        out->url = (mdit_str){ buf, url_prefix_len + raw_len };
    } else {
        out->url = out->text;
    }
    return true;
}

static bool match_http(mdit_lib_ctx *lib, mdit_arena *arena,
                       mdit_str text, size_t pos,
                       size_t scheme_len,
                       mdit_linkify_match *out)
{
    /* `pos` points at the start of the scheme. After the scheme,
     * expect `://`, then optional auth, host, port, path. */
    size_t i = pos + scheme_len + 1; /* past the colon */
    if (i + 1 >= text.len) return false;
    if (text.data[i] != '/' || text.data[i + 1] != '/') return false;
    i += 2;

    size_t auth = scan_auth(text.data + i, text.len - i);
    i += auth;

    size_t tld_off, tld_len;
    size_t host = scan_host(text.data + i, text.len - i, &tld_off, &tld_len);
    if (host == 0) return false;
    i += host;

    i += scan_port(text.data + i, text.len - i);
    i += scan_path(text.data + i, text.len - i);

    /* Build the schema literal "http:" / "https:" / "ftp:". */
    mdit_str schema = { text.data + pos, scheme_len + 1 };
    /* Emit the URL as the matched substring (no prefix). */
    return emit_match(lib, arena, schema, text, pos, i, NULL, 0, out);
}

static bool match_mailto(mdit_lib_ctx *lib, mdit_arena *arena,
                         mdit_str text, size_t pos,
                         size_t scheme_len,
                         mdit_linkify_match *out)
{
    size_t i = pos + scheme_len + 1; /* past the colon */
    size_t local = scan_email_local(text.data + i, text.len - i);
    if (local == 0) return false;
    i += local;
    if (i >= text.len || text.data[i] != '@') return false;
    ++i;
    size_t tld_off, tld_len;
    size_t host = scan_host(text.data + i, text.len - i, &tld_off, &tld_len);
    if (host == 0) return false;
    i += host;
    /* mailto schema doesn't append path. */
    mdit_str schema = MDIT_STR_LIT("mailto:");
    return emit_match(lib, arena, schema, text, pos, i, NULL, 0, out);
}

/* Bare email `local@host.tld`. Validated against the TLD list.
 * Boundary on the LEFT side: must be at start of text or preceded by
 * whitespace / punctuation that linkify-it allows (we approximate).
 */
static bool match_fuzzy_email(mdit_lib_ctx *lib, mdit_arena *arena,
                              mdit_str text, size_t pos,
                              mdit_linkify_match *out)
{
    /* We're called when text[pos] == '@'. Scan backward for the local
     * part, forward for the host. */
    if (pos == 0 || pos >= text.len) return false;
    size_t back = pos;
    while (back > 0) {
        unsigned char c = (unsigned char)text.data[back - 1];
        if (!is_email_local_rest(c)) break;
        --back;
    }
    if (back == pos) return false;
    /* The first char of the local part must satisfy `is_email_local_first`. */
    if (!is_email_local_first((unsigned char)text.data[back])) return false;

    /* Left-boundary test: must be start, whitespace, or punctuation
     * that matches upstream's `(^|[><\uff5c]|"|\(|<Z>|<Cc>)`. We
     * approximate as "any non-pseudo-letter ASCII or start-of-text". */
    if (back > 0) {
        unsigned char prev = (unsigned char)text.data[back - 1];
        if (is_pseudo_letter_byte(prev)) return false;
    }

    size_t i = pos + 1;
    size_t tld_off, tld_len;
    size_t host = scan_host(text.data + i, text.len - i, &tld_off, &tld_len);
    if (host == 0) return false;
    /* Validate TLD. */
    if (!tld_lookup(text.data + i + tld_off, tld_len)) return false;
    i += host;

    /* Build url = "mailto:" + raw. */
    static const char prefix[] = "mailto:";
    return emit_match(lib, arena, MDIT_STR_LIT("mailto:"), text, back, i,
                      prefix, sizeof prefix - 1, out);
}

/* Bare URL `host.tld[:port][/path]` or `www.host.tld...` — fuzzy. */
static bool match_fuzzy_link(mdit_lib_ctx *lib, mdit_arena *arena,
                             mdit_str text, size_t pos,
                             mdit_linkify_match *out)
{
    /* `pos` points at a `.` that might be a TLD boundary. Scan
     * backwards for the host's first label, forwards for additional
     * labels and TLD. */

    /* Walk back to the start of this label. */
    size_t back = pos;
    while (back > 0 &&
           (is_pseudo_letter_byte((unsigned char)text.data[back - 1]) ||
            text.data[back - 1] == '-')) {
        --back;
    }
    /* Then optionally walk further back through more `label.` runs. */
    while (back > 0 && text.data[back - 1] == '.') {
        size_t prev = back - 1;
        while (prev > 0 &&
               (is_pseudo_letter_byte((unsigned char)text.data[prev - 1]) ||
                text.data[prev - 1] == '-')) {
            --prev;
        }
        if (prev == back - 1) break; /* no label before `.` */
        back = prev;
    }
    /* The first character of the leftmost label must be a pseudo-letter
     * (no leading hyphen). */
    if (back >= text.len || !is_pseudo_letter_byte((unsigned char)text.data[back])) {
        return false;
    }
    /* Left boundary: at start or preceded by allowed character. */
    if (back > 0) {
        unsigned char prev = (unsigned char)text.data[back - 1];
        if (prev == '.' || prev == ':' || prev == '/' || prev == '\\' ||
            prev == '-' || prev == '_' || prev == '@') return false;
        if (is_pseudo_letter_byte(prev)) return false;
    }

    size_t tld_off, tld_len;
    size_t host = scan_host(text.data + back, text.len - back,
                            &tld_off, &tld_len);
    if (host == 0) return false;
    /* Must contain at least one `.` (host is multi-label). */
    bool has_dot = false;
    for (size_t i = 0; i < host; ++i) {
        if (text.data[back + i] == '.') { has_dot = true; break; }
    }
    if (!has_dot) return false;
    if (!tld_lookup(text.data + back + tld_off, tld_len)) return false;

    size_t i = back + host;
    i += scan_port(text.data + i, text.len - i);
    i += scan_path(text.data + i, text.len - i);

    static const char prefix[] = "http://";
    return emit_match(lib, arena, MDIT_STR_LIT(""), text, back, i,
                      prefix, sizeof prefix - 1, out);
}

/* Try every known matcher at position `pos`. Returns true if any
 * succeeded. */
static bool try_match_at(mdit_lib_ctx *lib, mdit_arena *arena,
                         mdit_str text, size_t pos,
                         mdit_linkify_match *out)
{
    /* Explicit scheme? */
    size_t scheme_len = scan_scheme(text.data + pos, text.len - pos);
    if (scheme_len > 0) {
        const char *sn = text.data + pos;
        if (is_http_scheme(sn, scheme_len)) {
            if (match_http(lib, arena, text, pos, scheme_len, out)) return true;
        }
        if (is_mailto_scheme(sn, scheme_len)) {
            if (match_mailto(lib, arena, text, pos, scheme_len, out)) return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------------
 * Public entry points
 * ------------------------------------------------------------------- */
static bool df_test(void *self, mdit_lib_ctx *lib, mdit_str text)
{
    (void)self;
    if (!df_pretest(NULL, lib, text)) return false;
    mdit_arena scratch;
    mdit_arena_init(&scratch, 0);
    mdit_linkify_match m;

    for (size_t i = 0; i < text.len; ++i) {
        if (try_match_at(lib, &scratch, text, i, &m)) {
            mdit_arena_destroy(lib, &scratch);
            return true;
        }
        if (text.data[i] == '@' &&
            match_fuzzy_email(lib, &scratch, text, i, &m)) {
            mdit_arena_destroy(lib, &scratch);
            return true;
        }
        if (text.data[i] == '.' &&
            match_fuzzy_link(lib, &scratch, text, i, &m)) {
            mdit_arena_destroy(lib, &scratch);
            return true;
        }
    }
    mdit_arena_destroy(lib, &scratch);
    return false;
}

static bool df_match_at_start(void *self, mdit_lib_ctx *lib, mdit_arena *arena,
                              mdit_str text, mdit_linkify_match *out)
{
    (void)self;
    if (text.len == 0) return false;
    if (try_match_at(lib, arena, text, 0, out) && out->index == 0) return true;
    /* Only explicit-scheme matches are returned by `match_at_start`
     * upstream — fuzzy matches require a non-letter prefix. */
    return false;
}

static size_t df_match_all(void *self, mdit_lib_ctx *lib, mdit_arena *arena,
                           mdit_str text, mdit_linkify_match **out)
{
    (void)self;
    *out = NULL;
    if (!df_pretest(NULL, lib, text)) return 0;

    /* Two-pass: count, then fill. */
    size_t count = 0;
    for (size_t pass = 0; pass < 2; ++pass) {
        if (pass == 1 && count > 0) {
            *out = (mdit_linkify_match *)mdit_arena_alloc(lib, arena,
                count * sizeof **out);
            if (*out == NULL) return 0;
        }
        size_t idx = 0;
        size_t i = 0;
        while (i < text.len) {
            mdit_linkify_match m;
            bool found = false;
            if (try_match_at(lib, arena, text, i, &m)) {
                found = true;
            } else if (text.data[i] == '@' &&
                       match_fuzzy_email(lib, arena, text, i, &m)) {
                found = true;
            } else if (text.data[i] == '.' &&
                       match_fuzzy_link(lib, arena, text, i, &m)) {
                found = true;
            }
            if (found) {
                if (pass == 1) (*out)[idx] = m;
                ++idx;
                i = m.last_index;
                if (i <= m.index) ++i;  /* defensive */
                continue;
            }
            ++i;
        }
        if (pass == 0) count = idx;
    }
    return count;
}

/* ---------------------------------------------------------------------
 * Singleton
 * ------------------------------------------------------------------- */
static const mdit_linkifier g_default = {
    .self           = NULL,
    .pretest        = df_pretest,
    .test           = df_test,
    .match_at_start = df_match_at_start,
    .match_all      = df_match_all,
};

const mdit_linkifier *mdit_linkifier_default(void)
{
    return &g_default;
}

bool mdit_linkifier_default_use_full_tlds(bool enable)
{
    bool prev = g_use_full_tlds;
    g_use_full_tlds = enable;
    return prev;
}

bool mdit_linkifier_default_full_tlds_enabled(void)
{
    return g_use_full_tlds;
}
