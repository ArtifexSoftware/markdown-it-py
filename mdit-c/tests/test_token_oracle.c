/*
 * test_token_oracle.c — end-to-end token-stream byte parity.
 *
 * Reads JSONL files written by `scripts/token_oracle.py` (one record
 * per line, where each record carries the input markdown and the
 * Python-rendered token tree as `json.dumps(token.as_dict(...),
 * ensure_ascii=False)`). For each row, we:
 *
 *   1. Parse `input` with a `mdit_md` configured per the row's
 *      `preset` / `options` / `enabled` / `disabled` metadata.
 *   2. Serialize the resulting tokens via `mdit_tokens_to_json`.
 *   3. Compare bytes against the row's `tokens` field.
 *
 * The block oracle covers HTML byte parity; this oracle catches token
 * shape differences that happen to render the same HTML (e.g. empty
 * children vs. None, markup field values, attribute order).
 *
 * The corpus lives at `mdit-c/tests/oracle/*.jsonl`; the source(s) we
 * run is controlled by the `SOURCES` array below. Any source not on
 * the list is skipped; new sources should be added as the C port
 * gains rule coverage.
 */
#include "mdit_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "json.h"
#include "linkifier.h"
#include "main.h"
#include "ruler.h"
#include "str.h"
#include "token.h"

/* ---------------------------------------------------------------------
 * Path to the oracle directory. CMake injects it via the
 * `MDIT_TEST_ORACLE_DIR` macro; fall back to a relative path so the
 * binary still works when launched from the workspace root.
 * ------------------------------------------------------------------- */
#ifndef MDIT_TEST_ORACLE_DIR
#define MDIT_TEST_ORACLE_DIR "mdit-c/tests/oracle"
#endif

/* ---------------------------------------------------------------------
 * Sources to drive end-to-end. Each entry maps to a `<name>.jsonl`
 * file; the runner walks every line of every listed file.
 *
 * NOTE: Sources covering plugin rules the C port hasn't ported yet
 * (e.g. strikethrough, tasklists, alerts) are intentionally absent.
 * ------------------------------------------------------------------- */
static const char *const SOURCES[] = {
    "commonmark_spec",
    "commonmark_spec_doc",
    "normalize",
    "commonmark_extras",
    "tables",
    "typographer",
    "smartquotes",
    "linkify",
    "proto",
    "xss",
    "issue-fixes",
    "strikethrough",
    "strikethrough_single_tilde",
};
static const size_t N_SOURCES = sizeof SOURCES / sizeof SOURCES[0];

/* ---------------------------------------------------------------------
 * Minimal JSON value parser
 *
 * The oracle JSON is well-formed and escape-conservative (Python's
 * `json.dumps(ensure_ascii=False)`), so we don't need full RFC 8259
 * coverage. We support:
 *   - whitespace skipping
 *   - skipping a value (object / array / string / number / literal)
 *   - extracting a string value (with backslash decode)
 *   - finding a top-level field in an object
 *   - extracting a *raw* substring of a value (byte-for-byte)
 *
 * All offsets are byte offsets into a single line buffer.
 * ------------------------------------------------------------------- */
typedef struct {
    const char *data;
    size_t      len;
} jv_view;

static size_t skip_ws(const char *s, size_t n, size_t i)
{
    while (i < n) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i;
        else break;
    }
    return i;
}

/* Forward decl. */
static size_t skip_value(const char *s, size_t n, size_t i);

static size_t skip_string(const char *s, size_t n, size_t i)
{
    /* `s[i] == '"'`. */
    ++i;
    while (i < n) {
        char c = s[i];
        if (c == '\\') {
            if (i + 1 >= n) return n;
            /* `\u` consumes 4 hex digits; everything else is 1 byte. */
            if (s[i + 1] == 'u') i += 6;
            else                 i += 2;
            continue;
        }
        if (c == '"') return i + 1;
        ++i;
    }
    return n;
}

static size_t skip_object(const char *s, size_t n, size_t i)
{
    ++i;  /* past `{` */
    i = skip_ws(s, n, i);
    if (i < n && s[i] == '}') return i + 1;
    while (i < n) {
        i = skip_ws(s, n, i);
        if (i >= n || s[i] != '"') return n;
        i = skip_string(s, n, i);
        i = skip_ws(s, n, i);
        if (i >= n || s[i] != ':') return n;
        ++i;
        i = skip_ws(s, n, i);
        i = skip_value(s, n, i);
        i = skip_ws(s, n, i);
        if (i < n && s[i] == ',') { ++i; continue; }
        if (i < n && s[i] == '}') return i + 1;
        return n;
    }
    return n;
}

static size_t skip_array(const char *s, size_t n, size_t i)
{
    ++i;  /* past `[` */
    i = skip_ws(s, n, i);
    if (i < n && s[i] == ']') return i + 1;
    while (i < n) {
        i = skip_ws(s, n, i);
        i = skip_value(s, n, i);
        i = skip_ws(s, n, i);
        if (i < n && s[i] == ',') { ++i; continue; }
        if (i < n && s[i] == ']') return i + 1;
        return n;
    }
    return n;
}

static size_t skip_literal(const char *s, size_t n, size_t i)
{
    while (i < n) {
        char c = s[i];
        if ((c >= '0' && c <= '9') || c == '-' || c == '+' ||
            c == '.' || c == 'e' || c == 'E' ||
            (c >= 'a' && c <= 'z')) {
            ++i;
            continue;
        }
        break;
    }
    return i;
}

static size_t skip_value(const char *s, size_t n, size_t i)
{
    if (i >= n) return n;
    char c = s[i];
    if (c == '"') return skip_string(s, n, i);
    if (c == '{') return skip_object(s, n, i);
    if (c == '[') return skip_array(s,  n, i);
    return skip_literal(s, n, i);
}

/* Find field `name` in the top-level object and return a view into
 * its raw value bytes (excluding leading whitespace, but including
 * the value's quotes / brackets). Returns false on miss. */
static bool find_field(const char *s, size_t n,
                       const char *name, size_t name_len,
                       jv_view *out)
{
    size_t i = skip_ws(s, n, 0);
    if (i >= n || s[i] != '{') return false;
    ++i;
    while (i < n) {
        i = skip_ws(s, n, i);
        if (i >= n) return false;
        if (s[i] == '}') return false;
        if (s[i] != '"') return false;
        size_t key_start = i + 1;
        size_t key_end_p1 = skip_string(s, n, i);
        size_t key_end = key_end_p1 - 1;  /* past the closing quote */
        i = skip_ws(s, n, key_end_p1);
        if (i >= n || s[i] != ':') return false;
        ++i;
        i = skip_ws(s, n, i);
        size_t val_start = i;
        size_t val_end = skip_value(s, n, i);
        if (val_end - val_start == 0) return false;
        if (key_end - key_start == name_len &&
            memcmp(s + key_start, name, name_len) == 0) {
            out->data = s + val_start;
            out->len  = val_end - val_start;
            return true;
        }
        i = skip_ws(s, n, val_end);
        if (i < n && s[i] == ',') { ++i; continue; }
        return false;
    }
    return false;
}

/* Decode the JSON string starting at `v.data` (which must point at the
 * opening `"`) into a heap buffer. Returns NULL on alloc failure or
 * malformed input. The returned buffer is NUL-terminated for
 * convenience but MAY contain embedded NULs; use `*out_len` for size.
 */
static char *decode_string(jv_view v, size_t *out_len)
{
    if (v.len < 2 || v.data[0] != '"' || v.data[v.len - 1] != '"') {
        return NULL;
    }
    /* Worst case: every byte is literal. */
    char *out = (char *)malloc(v.len);
    if (out == NULL) return NULL;
    size_t w = 0;
    size_t i = 1;
    size_t end = v.len - 1;
    while (i < end) {
        unsigned char c = (unsigned char)v.data[i];
        if (c != '\\') {
            out[w++] = (char)c;
            ++i;
            continue;
        }
        if (i + 1 >= end) { free(out); return NULL; }
        char esc = v.data[i + 1];
        switch (esc) {
            case '"':  out[w++] = '"';  i += 2; break;
            case '\\': out[w++] = '\\'; i += 2; break;
            case '/':  out[w++] = '/';  i += 2; break;
            case 'b':  out[w++] = '\b'; i += 2; break;
            case 'f':  out[w++] = '\f'; i += 2; break;
            case 'n':  out[w++] = '\n'; i += 2; break;
            case 'r':  out[w++] = '\r'; i += 2; break;
            case 't':  out[w++] = '\t'; i += 2; break;
            case 'u': {
                if (i + 5 >= end) { free(out); return NULL; }
                unsigned int cp = 0;
                for (int k = 0; k < 4; ++k) {
                    char h = v.data[i + 2 + k];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                    else { free(out); return NULL; }
                }
                /* Handle surrogate pair. */
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 11 < end &&
                    v.data[i + 6] == '\\' && v.data[i + 7] == 'u') {
                    unsigned int low = 0;
                    for (int k = 0; k < 4; ++k) {
                        char h = v.data[i + 8 + k];
                        low <<= 4;
                        if (h >= '0' && h <= '9') low |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') low |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') low |= (unsigned)(h - 'A' + 10);
                        else { free(out); return NULL; }
                    }
                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        i += 12;
                    } else {
                        i += 6;
                    }
                } else {
                    i += 6;
                }
                /* UTF-8 encode `cp`. */
                if (cp < 0x80) {
                    out[w++] = (char)cp;
                } else if (cp < 0x800) {
                    out[w++] = (char)(0xC0 | (cp >> 6));
                    out[w++] = (char)(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    out[w++] = (char)(0xE0 | (cp >> 12));
                    out[w++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    out[w++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    out[w++] = (char)(0xF0 | (cp >> 18));
                    out[w++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                    out[w++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    out[w++] = (char)(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: free(out); return NULL;
        }
    }
    out[w] = '\0';
    *out_len = w;
    return out;
}

/* Test whether a JSON literal view equals `true`, `false`, etc. */
static bool jv_is_true (jv_view v) { return v.len == 4 && memcmp(v.data, "true",  4) == 0; }
static bool jv_is_false(jv_view v) { return v.len == 5 && memcmp(v.data, "false", 5) == 0; }
static bool jv_is_str  (jv_view v, const char *s, size_t n)
{
    return v.len == n + 2 && v.data[0] == '"' && v.data[v.len - 1] == '"' &&
           memcmp(v.data + 1, s, n) == 0;
}

/* Iterate string members of an array view: each yields a (data, len)
 * pointer into the original buffer (i.e. the substring between the
 * member's quotes, *with* JSON escapes intact — callers must decode
 * if they care). Return true while there's another member. */
typedef struct {
    const char *data;
    size_t      len;
    size_t      pos;  /* position within data */
} jv_str_iter;

static jv_str_iter jv_str_iter_init(jv_view arr)
{
    jv_str_iter it = { arr.data, arr.len, 0 };
    /* `data[0] == '['`. */
    if (it.len > 0 && it.data[0] == '[') it.pos = 1;
    return it;
}

static bool jv_str_iter_next(jv_str_iter *it, jv_view *out)
{
    it->pos = skip_ws(it->data, it->len, it->pos);
    if (it->pos >= it->len) return false;
    if (it->data[it->pos] == ']') return false;
    if (it->data[it->pos] == ',') { ++it->pos; it->pos = skip_ws(it->data, it->len, it->pos); }
    if (it->pos >= it->len || it->data[it->pos] != '"') return false;
    size_t start = it->pos + 1;
    size_t end_p1 = skip_string(it->data, it->len, it->pos);
    out->data = it->data + start;
    out->len  = end_p1 - 1 - start;
    it->pos = end_p1;
    return true;
}

/* ---------------------------------------------------------------------
 * Per-row runtime configuration: extracts what we need from a JSONL
 * record and applies it to a freshly-initialised `mdit_md`.
 * ------------------------------------------------------------------- */
typedef struct {
    bool        in_commonmark;  /* preset == "commonmark" */
    bool        opt_html;
    bool        opt_xhtml;
    bool        opt_breaks;
    bool        opt_linkify;
    bool        opt_typographer;
    bool        opt_strikethrough_single_tilde;
    bool        has_lang_prefix;
    mdit_str    lang_prefix;
    /* enabled / disabled lists are the raw JSON array views; we
     * iterate over them to apply ruler enable/disable directly. */
    jv_view     enabled;
    jv_view     disabled;
} row_config;

static void row_config_extract(const char *line, size_t len, row_config *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    jv_view v;
    if (find_field(line, len, "preset", 6, &v) &&
        jv_is_str(v, "commonmark", 10)) {
        cfg->in_commonmark = true;
    }
    if (find_field(line, len, "options", 7, &v) && v.len >= 2 && v.data[0] == '{') {
        jv_view sub;
        if (find_field(v.data, v.len, "html", 4, &sub) && jv_is_true(sub)) cfg->opt_html = true;
        if (find_field(v.data, v.len, "xhtmlOut", 8, &sub) && jv_is_true(sub)) cfg->opt_xhtml = true;
        if (find_field(v.data, v.len, "breaks", 6, &sub) && jv_is_true(sub)) cfg->opt_breaks = true;
        if (find_field(v.data, v.len, "linkify", 7, &sub) && jv_is_true(sub)) cfg->opt_linkify = true;
        if (find_field(v.data, v.len, "typographer", 11, &sub) && jv_is_true(sub)) cfg->opt_typographer = true;
        if (find_field(v.data, v.len, "strikethrough_single_tilde", 26, &sub) && jv_is_true(sub)) cfg->opt_strikethrough_single_tilde = true;
        if (find_field(v.data, v.len, "langPrefix", 10, &sub) &&
            sub.len >= 2 && sub.data[0] == '"' && sub.data[sub.len - 1] == '"') {
            /* Current fixture metadata uses simple unescaped strings
             * (`""` and `"language-"`). Store a borrowed view into the
             * JSONL line; the line stays live for the duration of this
             * parse. */
            cfg->has_lang_prefix = true;
            cfg->lang_prefix = (mdit_str){ sub.data + 1, sub.len - 2 };
        }
        (void)jv_is_false; /* silence unused-warn if no field uses it */
    }
    if (find_field(line, len, "enabled", 7, &v) && v.len >= 2 && v.data[0] == '[') {
        cfg->enabled = v;
    }
    if (find_field(line, len, "disabled", 8, &v) && v.len >= 2 && v.data[0] == '[') {
        cfg->disabled = v;
    }
}

/* Apply the row config to an already-initialised `mdit_md`. */
static void apply_row_config(mdit_md *md, const row_config *cfg)
{
    if (cfg->in_commonmark) {
        /* Upstream commonmark preset options. */
        md->options.max_nesting = 20;
        md->options.html        = true;
        md->options.xhtml_out   = true;
    }
    md->options.html        = cfg->opt_html || md->options.html;
    md->options.xhtml_out   = cfg->opt_xhtml || md->options.xhtml_out;
    md->options.breaks      = cfg->opt_breaks;
    md->options.linkify     = cfg->opt_linkify;
    md->options.typographer = cfg->opt_typographer;
    md->options.strikethrough_single_tilde = cfg->opt_strikethrough_single_tilde;
    if (cfg->has_lang_prefix) {
        md->options.lang_prefix = cfg->lang_prefix;
    }
    if (cfg->opt_linkify) {
        mdit_md_set_linkifier(md, mdit_linkifier_default());
    }

    /* The C port has GFM `table` and inline `strikethrough` on by
     * default, matching upstream's `default` preset. Upstream's
     * `commonmark` preset, however, enables neither, so we disable
     * both explicitly when the test row is tagged for commonmark. */
    if (cfg->in_commonmark) {
        mdit_str table = MDIT_STR_LIT("table");
        (void)mdit_ruler_disable(md->block.ruler, &table, 1, true);
        mdit_str strike = MDIT_STR_LIT("strikethrough");
        (void)mdit_ruler_disable(md->inline_p.ruler,  &strike, 1, true);
        (void)mdit_ruler_disable(md->inline_p.ruler2, &strike, 1, true);
    }

    /* Apply explicit `disabled` rules (try every chain; ignore misses). */
    jv_str_iter it = jv_str_iter_init(cfg->disabled);
    jv_view name_v;
    while (jv_str_iter_next(&it, &name_v)) {
        mdit_str name = { name_v.data, name_v.len };
        (void)mdit_ruler_disable(md->core.ruler,     &name, 1, true);
        (void)mdit_ruler_disable(md->block.ruler,    &name, 1, true);
        (void)mdit_ruler_disable(md->inline_p.ruler, &name, 1, true);
    }
    /* Apply explicit `enabled` rules similarly. */
    it = jv_str_iter_init(cfg->enabled);
    while (jv_str_iter_next(&it, &name_v)) {
        mdit_str name = { name_v.data, name_v.len };
        (void)mdit_ruler_enable(md->core.ruler,     &name, 1, true);
        (void)mdit_ruler_enable(md->block.ruler,    &name, 1, true);
        (void)mdit_ruler_enable(md->inline_p.ruler, &name, 1, true);
    }
}

/* ---------------------------------------------------------------------
 * File reading
 * ------------------------------------------------------------------- */
static char *read_whole_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (buf == NULL) { fclose(f); return NULL; }
    size_t r = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (r != (size_t)n) { free(buf); return NULL; }
    buf[n] = '\0';
    *out_len = (size_t)n;
    return buf;
}

/* Iterate over `\n`-separated lines in a buffer. Returns true while
 * there's a non-empty line; sets `*line` / `*line_len` to point into
 * the buffer (no allocation). */
typedef struct {
    const char *buf;
    size_t      len;
    size_t      pos;
} line_iter;

static bool line_iter_next(line_iter *it, const char **line, size_t *line_len)
{
    while (it->pos < it->len) {
        size_t start = it->pos;
        while (it->pos < it->len && it->buf[it->pos] != '\n') ++it->pos;
        size_t end = it->pos;
        if (it->pos < it->len) ++it->pos;  /* past the `\n` */
        /* Strip a trailing `\r`. */
        if (end > start && it->buf[end - 1] == '\r') --end;
        if (end > start) {
            *line = it->buf + start;
            *line_len = end - start;
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------------
 * Reporting
 * ------------------------------------------------------------------- */
static size_t g_total_cases    = 0;
static size_t g_total_failures = 0;

/* Print a unified-diff-ish marker for a mismatch. `line` is the
 * JSONL line for context (we extract `id`/`title`). */
static void report_mismatch(const char *source,
                            const char *line, size_t line_len,
                            const char *got, size_t got_len,
                            const char *want, size_t want_len)
{
    jv_view title_v = { "", 0 };
    jv_view id_v    = { "", 0 };
    (void)find_field(line, line_len, "title", 5, &title_v);
    (void)find_field(line, line_len, "id",    2, &id_v);

    fprintf(stderr,
            "  [%s] %.*s id=%.*s\n",
            source,
            (int)title_v.len, title_v.data,
            (int)id_v.len,    id_v.data);
    /* Limit dumped sizes so we don't drown logs. */
    size_t cap = 600;
    fprintf(stderr,
            "    got:  %.*s%s\n"
            "    want: %.*s%s\n",
            (int)(got_len  > cap ? cap : got_len ), got,
            got_len  > cap ? "..." : "",
            (int)(want_len > cap ? cap : want_len), want,
            want_len > cap ? "..." : "");
}

static void run_source(const char *source)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.jsonl", MDIT_TEST_ORACLE_DIR, source);

    size_t buf_len = 0;
    char *buf = read_whole_file(path, &buf_len);
    if (buf == NULL) {
        char msg[1280];
        snprintf(msg, sizeof msg,
                 "could not open oracle %s — set MDIT_TEST_ORACLE_DIR or run "
                 "from the repo root", path);
        MDIT_FAIL(msg);
        return;
    }

    line_iter it = { buf, buf_len, 0 };
    const char *line; size_t line_len;
    size_t case_idx = 0;
    while (line_iter_next(&it, &line, &line_len)) {
        ++case_idx;
        ++g_total_cases;

        jv_view input_v, tokens_v;
        if (!find_field(line, line_len, "input",  5, &input_v) ||
            !find_field(line, line_len, "tokens", 6, &tokens_v)) {
            ++g_total_failures;
            fprintf(stderr, "  [%s] case %zu: missing input/tokens field\n",
                    source, case_idx);
            continue;
        }

        size_t input_len = 0;
        char *input_str = decode_string(input_v, &input_len);
        if (input_str == NULL) {
            ++g_total_failures;
            fprintf(stderr, "  [%s] case %zu: invalid input encoding\n",
                    source, case_idx);
            continue;
        }

        row_config cfg;
        row_config_extract(line, line_len, &cfg);

        mdit_arena a;
        mdit_arena_init(&a, 0);
        mdit_md md;
        if (!mdit_md_init(&md, &a)) {
            ++g_total_failures;
            free(input_str);
            mdit_arena_destroy(&a);
            fprintf(stderr, "  [%s] case %zu: mdit_md_init failed\n",
                    source, case_idx);
            continue;
        }
        apply_row_config(&md, &cfg);

        /* Save / restore the global linkifier toggle so cases stay
         * independent (matches block oracle hygiene). */
        bool prev_full_tlds = mdit_linkifier_default_full_tlds_enabled();
        mdit_linkifier_default_use_full_tlds(false);

        mdit_vec_token tokens;
        mdit_vec_token_init(&tokens, &a);
        bool ok = mdit_md_parse(&md, (mdit_str){ input_str, input_len },
                                NULL, &tokens);
        if (!ok) {
            ++g_total_failures;
            mdit_md_destroy(&md);
            mdit_linkifier_default_use_full_tlds(prev_full_tlds);
            free(input_str);
            mdit_arena_destroy(&a);
            fprintf(stderr, "  [%s] case %zu: mdit_md_parse failed\n",
                    source, case_idx);
            continue;
        }

        mdit_buf out;
        mdit_buf_init(&out);
        if (!mdit_tokens_to_json(tokens.data, tokens.len, &out)) {
            ++g_total_failures;
            mdit_buf_destroy(&out);
            mdit_md_destroy(&md);
            mdit_linkifier_default_use_full_tlds(prev_full_tlds);
            free(input_str);
            mdit_arena_destroy(&a);
            fprintf(stderr, "  [%s] case %zu: token serialization failed\n",
                    source, case_idx);
            continue;
        }

        if (out.len != tokens_v.len ||
            memcmp(out.data ? out.data : "", tokens_v.data, tokens_v.len) != 0) {
            ++g_total_failures;
            report_mismatch(source, line, line_len,
                            out.data ? out.data : "", out.len,
                            tokens_v.data, tokens_v.len);
        }

        mdit_buf_destroy(&out);
        mdit_md_destroy(&md);
        mdit_linkifier_default_use_full_tlds(prev_full_tlds);
        free(input_str);
        mdit_arena_destroy(&a);
    }
    free(buf);
}

MDIT_TEST(token_oracle_byte_parity)
{
    g_total_cases    = 0;
    g_total_failures = 0;
    for (size_t i = 0; i < N_SOURCES; ++i) {
        run_source(SOURCES[i]);
    }
    if (g_total_failures != 0) {
        char msg[256];
        snprintf(msg, sizeof msg,
                 "%zu / %zu token-oracle cases failed",
                 g_total_failures, g_total_cases);
        MDIT_FAIL(msg);
    }
    fprintf(stderr, "  token oracle: %zu cases pass\n", g_total_cases);
}

#define MDIT_TEST_REGISTRY \
    MDIT_TEST_LIST_ENTRY(token_oracle_byte_parity)

#include "mdit_test_main.h"
