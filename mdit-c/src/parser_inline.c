/*
 * parser_inline.c — inline tokenizer driver + built-in rules.
 */
#include "parser_inline.h"

#include <string.h>

#include "env.h"
#include "link_helpers.h"
#include "linkifier.h"
#include "main.h"
#include "normalize_url.h"
#include "parse_link_label.h"
#include "utf.h"

/* Default terminator set, ported from parser_inline.py. */
static const char DEFAULT_TERMINATORS[] =
    "\n!#$%&*+-:<=>@[\\]^_`{}~";

static bool ascii_is_space(unsigned char c) { return c == ' ' || c == '\t'; }

/* ---------------------------------------------------------------------
 * Built-in rule: text (catch-all).
 *
 * Advances state.pos until the next terminator byte (or end of input)
 * and appends the consumed range to state.pending. Returns false if
 * nothing was consumed (so the caller falls through to single-byte
 * advance).
 * ------------------------------------------------------------------- */
static bool inline_text(mdit_state_inline *state, bool silent)
{
    size_t pos = state->pos;
    size_t pos_max = state->pos_max;
    const bool *term = state->md->inline_p.terminator_ascii;
    while (pos < pos_max) {
        unsigned char c = (unsigned char)state->src.data[pos];
        if (c < 0x80 && term[c]) break;
        ++pos;
    }
    if (pos == state->pos) return false;
    if (!silent) {
        (void)mdit_state_inline_append_pending(state,
            state->src.data + state->pos, pos - state->pos);
    }
    state->pos = pos;
    return true;
}

/* ---------------------------------------------------------------------
 * Built-in rule: escape (\X backslash escapes).
 *
 * Emits ``text_special`` for the escaped char (or ``hardbreak`` for
 * "\\\n"). The downstream core ``text_join`` rule converts
 * ``text_special`` to ``text`` and merges runs.
 * ------------------------------------------------------------------- */
static bool _is_escapable(unsigned char c)
{
    /* ASCII punctuation per CommonMark. */
    switch (c) {
        case '!': case '"': case '#': case '$': case '%': case '&':
        case '\'': case '(': case ')': case '*': case '+': case ',':
        case '-': case '.': case '/': case ':': case ';': case '<':
        case '=': case '>': case '?': case '@': case '[': case '\\':
        case ']': case '^': case '_': case '`': case '{': case '|':
        case '}': case '~':
            return true;
        default:
            return false;
    }
}

static bool inline_escape(mdit_state_inline *state, bool silent)
{
    size_t pos = state->pos;
    size_t maximum = state->pos_max;
    if (pos >= maximum) return false;
    if (state->src.data[pos] != '\\') return false;
    ++pos;
    if (pos >= maximum) return false;

    unsigned char c1 = (unsigned char)state->src.data[pos];

    /* "\<newline>" -> hard break + skip leading whitespace. */
    if (c1 == '\n') {
        if (!silent) {
            (void)mdit_state_inline_push(state,
                MDIT_STR_LIT("hardbreak"), MDIT_STR_LIT("br"), 0);
        }
        ++pos;
        while (pos < maximum &&
               ascii_is_space((unsigned char)state->src.data[pos])) ++pos;
        state->pos = pos;
        return true;
    }

    if (!silent) {
        /* For escapable punctuation, emit just the escaped char as
         * text_special; otherwise emit "\X" (preserving the backslash). */
        bool escapable = _is_escapable(c1);
        const char *content_start;
        size_t      content_len;
        const char *markup_start = state->src.data + state->pos; /* '\' */
        size_t      markup_len   = 2;
        if (escapable) {
            content_start = state->src.data + pos;
            content_len   = 1;
        } else {
            content_start = markup_start;
            content_len   = 2;
        }

        mdit_token *t = mdit_state_inline_push(state,
            MDIT_STR_LIT("text_special"), MDIT_STR_LIT(""), 0);
        if (t == NULL) return false;
        char *cbuf = (char *)mdit_arena_alloc(state->arena, content_len);
        memcpy(cbuf, content_start, content_len);
        t->content.data = cbuf;
        t->content.len  = content_len;
        char *mbuf = (char *)mdit_arena_alloc(state->arena, markup_len);
        memcpy(mbuf, markup_start, markup_len);
        t->markup.data = mbuf;
        t->markup.len  = markup_len;
        t->info = MDIT_STR_LIT("escape");
    }

    state->pos = pos + 1;
    return true;
}

/* ---------------------------------------------------------------------
 * Built-in rule: backtick (`code` spans).
 *
 * Scan opener marker length, look for closer of identical length.
 * Strip a single leading + trailing space if both present and content
 * isn't all whitespace. Newlines inside become regular spaces.
 *
 * Skipping the cache (``state.backticks``/``backticksScanned``)
 * upstream uses for backtracking optimization. The naive scan is O(n²)
 * worst case, but in practice fine for non-pathological input. We can
 * add the cache later if profiling shows it matters.
 * ------------------------------------------------------------------- */
static bool inline_backticks(mdit_state_inline *state, bool silent)
{
    size_t pos = state->pos;
    size_t pos_max = state->pos_max;
    if (pos >= pos_max) return false;
    if (state->src.data[pos] != '`') return false;

    size_t start = pos;
    ++pos;
    while (pos < pos_max && state->src.data[pos] == '`') ++pos;
    size_t opener_len = pos - start;
    size_t scan = pos;

    while (scan < pos_max) {
        /* Find next '`'. */
        const char *hit = (const char *)memchr(state->src.data + scan,
                                               '`', pos_max - scan);
        if (hit == NULL) break;
        size_t match_start = (size_t)(hit - state->src.data);
        size_t match_end = match_start + 1;
        while (match_end < pos_max && state->src.data[match_end] == '`') {
            ++match_end;
        }
        size_t closer_len = match_end - match_start;
        if (closer_len == opener_len) {
            if (!silent) {
                /* Build content: replace '\n' with ' '. */
                size_t content_len = match_start - pos;
                char *content = (char *)mdit_arena_alloc(state->arena,
                                                        content_len);
                for (size_t i = 0; i < content_len; ++i) {
                    char c = state->src.data[pos + i];
                    content[i] = (c == '\n') ? ' ' : c;
                }
                /* Strip single leading+trailing space if (a) both
                 * present and (b) not all-whitespace. */
                size_t lo = 0, hi = content_len;
                if (content_len >= 2 &&
                    content[0] == ' ' && content[content_len - 1] == ' ') {
                    bool all_ws = true;
                    for (size_t i = 0; i < content_len; ++i) {
                        if (content[i] != ' ') { all_ws = false; break; }
                    }
                    if (!all_ws) { lo = 1; hi = content_len - 1; }
                }
                mdit_token *t = mdit_state_inline_push(state,
                    MDIT_STR_LIT("code_inline"), MDIT_STR_LIT("code"), 0);
                if (t == NULL) return false;
                t->content.data = content + lo;
                t->content.len  = hi - lo;
                char *mbuf = (char *)mdit_arena_alloc(state->arena, opener_len);
                memcpy(mbuf, state->src.data + start, opener_len);
                t->markup.data = mbuf;
                t->markup.len  = opener_len;
            }
            state->pos = match_end;
            return true;
        }
        scan = match_end;
    }

    /* No matching closer — fall through to "literal backticks". */
    if (!silent) {
        (void)mdit_state_inline_append_pending(state,
            state->src.data + start, opener_len);
    }
    state->pos = start + opener_len;
    return true;
}

/* ---------------------------------------------------------------------
 * Built-in rule: entity (&name; &#decimal; &#xHEX;).
 *
 * Hand-coded scanner replaces upstream's two regex patterns. Numeric
 * entities use ``mdit_is_valid_entity_code`` (replaced by U+FFFD on
 * invalid). Named entities consult ``mdit_entity_lookup`` against the
 * pre-built HTML5 entity table.
 * ------------------------------------------------------------------- */
#include "entities.h"
#include "escape.h"

static bool emit_text_special_str(mdit_state_inline *state,
                                  const char *content, size_t content_len,
                                  const char *markup,  size_t markup_len)
{
    mdit_token *t = mdit_state_inline_push(state,
        MDIT_STR_LIT("text_special"), MDIT_STR_LIT(""), 0);
    if (t == NULL) return false;
    char *cbuf = (char *)mdit_arena_alloc(state->arena, content_len);
    memcpy(cbuf, content, content_len);
    t->content.data = cbuf; t->content.len = content_len;
    char *mbuf = (char *)mdit_arena_alloc(state->arena, markup_len);
    memcpy(mbuf, markup, markup_len);
    t->markup.data = mbuf; t->markup.len = markup_len;
    t->info = MDIT_STR_LIT("entity");
    return true;
}

static bool inline_entity(mdit_state_inline *state, bool silent)
{
    size_t pos = state->pos;
    size_t pos_max = state->pos_max;
    if (pos >= pos_max) return false;
    if (state->src.data[pos] != '&') return false;
    if (pos + 1 >= pos_max) return false;

    if (state->src.data[pos + 1] == '#') {
        /* Numeric: ^&#((?:x[a-f0-9]{1,6}|[0-9]{1,7})); */
        size_t s = pos + 2;
        bool   hex = false;
        if (s < pos_max && (state->src.data[s] == 'x' || state->src.data[s] == 'X')) {
            hex = true; ++s;
        }
        size_t digits_start = s;
        size_t max_digits = hex ? 6 : 7;
        while (s < pos_max && (s - digits_start) < max_digits) {
            unsigned char c = (unsigned char)state->src.data[s];
            bool ok = hex
                ? ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                   (c >= 'A' && c <= 'F'))
                :  (c >= '0' && c <= '9');
            if (!ok) break;
            ++s;
        }
        if (s == digits_start) return false;
        if (s >= pos_max || state->src.data[s] != ';') return false;
        if (silent) {
            state->pos = s + 1;
            return true;
        }
        /* Parse the integer. */
        uint32_t code = 0;
        for (size_t i = digits_start; i < s; ++i) {
            unsigned char c = (unsigned char)state->src.data[i];
            uint32_t d = 0;
            if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
            code = code * (hex ? 16u : 10u) + d;
        }
        if (!mdit_is_valid_entity_code(code)) code = 0xFFFDu;
        char utf8[4];
        size_t utf8_len;
        if (code < 0x80u) {
            utf8[0] = (char)code;
            utf8_len = 1;
        } else if (code < 0x800u) {
            utf8[0] = (char)(0xC0u | (code >> 6));
            utf8[1] = (char)(0x80u | (code & 0x3Fu));
            utf8_len = 2;
        } else if (code < 0x10000u) {
            utf8[0] = (char)(0xE0u | (code >> 12));
            utf8[1] = (char)(0x80u | ((code >> 6) & 0x3Fu));
            utf8[2] = (char)(0x80u | (code & 0x3Fu));
            utf8_len = 3;
        } else {
            utf8[0] = (char)(0xF0u | (code >> 18));
            utf8[1] = (char)(0x80u | ((code >> 12) & 0x3Fu));
            utf8[2] = (char)(0x80u | ((code >> 6) & 0x3Fu));
            utf8[3] = (char)(0x80u | (code & 0x3Fu));
            utf8_len = 4;
        }
        size_t markup_len = (s + 1) - pos;
        if (!emit_text_special_str(state,
                utf8, utf8_len,
                state->src.data + pos, markup_len)) return false;
        state->pos = s + 1;
        return true;
    }

    /* Named: ^&([a-z][a-z0-9]{1,31}); (case-insensitive lead). */
    size_t s = pos + 1;
    size_t name_start = s;
    unsigned char first = (unsigned char)state->src.data[s];
    if (!((first >= 'a' && first <= 'z') ||
          (first >= 'A' && first <= 'Z'))) return false;
    ++s;
    while (s < pos_max && (s - name_start) < 32) {
        unsigned char c = (unsigned char)state->src.data[s];
        bool ok = (c >= 'a' && c <= 'z') ||
                  (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9');
        if (!ok) break;
        ++s;
    }
    /* Need 1..31 chars after the lead. */
    size_t name_len = s - name_start;
    if (name_len < 2 || name_len > 32) return false;
    if (s >= pos_max || state->src.data[s] != ';') return false;
    /* Lookup against entity table (case-sensitive — matches Python). */
    const char *value = NULL;
    size_t value_len = 0;
    if (!mdit_entity_lookup(state->src.data + name_start, name_len,
                            &value, &value_len)) {
        return false;
    }
    if (silent) {
        state->pos = s + 1;
        return true;
    }
    size_t markup_len = (s + 1) - pos;
    if (!emit_text_special_str(state,
            value, value_len,
            state->src.data + pos, markup_len)) return false;
    state->pos = s + 1;
    return true;
}

/* ---------------------------------------------------------------------
 * Built-in rule: autolink (`<scheme:rest>` / `<email>`).
 *
 * Direct port of rules_inline/autolink.py. Hand scanners replace the
 * two upstream regexes:
 *
 *   AUTOLINK_RE: ^([a-zA-Z][a-zA-Z0-9+.-]{1,31}):([^<>\x00-\x20]*)$
 *   EMAIL_RE   : ^local@host(.host)*$  (RFC-ish, see python source)
 * ------------------------------------------------------------------- */
static bool autolink_url_ok(const char *s, size_t n)
{
    /* Scheme: [a-zA-Z][a-zA-Z0-9+.-]{1,31} */
    if (n == 0) return false;
    unsigned char c = (unsigned char)s[0];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return false;
    size_t i = 1;
    while (i < n) {
        c = (unsigned char)s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '+' || c == '.' || c == '-';
        if (!ok) break;
        ++i;
    }
    size_t scheme_len = i; /* includes lead char */
    if (scheme_len < 2 || scheme_len > 32) return false;
    if (i >= n || s[i] != ':') return false;
    ++i;
    /* Tail: [^<>\x00-\x20]* */
    while (i < n) {
        c = (unsigned char)s[i];
        if (c <= 0x20 || c == '<' || c == '>') return false;
        ++i;
    }
    return true;
}

static bool autolink_email_local_char(unsigned char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9')) return true;
    switch (c) {
        case '.': case '!': case '#': case '$': case '%': case '&':
        case '\'': case '*': case '+': case '/': case '=': case '?':
        case '^': case '_': case '`': case '{': case '|': case '}':
        case '~': case '-':
            return true;
        default:
            return false;
    }
}

static bool autolink_email_ok(const char *s, size_t n)
{
    /* local@host(.host)* — host = [a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])? */
    size_t i = 0;
    size_t local_start = 0;
    while (i < n && autolink_email_local_char((unsigned char)s[i])) ++i;
    if (i == local_start) return false;
    if (i >= n || s[i] != '@') return false;
    ++i;
    /* Each host label. */
    bool first_label = true;
    while (i < n) {
        size_t label_start = i;
        unsigned char c = (unsigned char)s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9'))) return false;
        ++i;
        size_t mid_start = i;
        while (i < n && i - label_start < 63) {
            c = (unsigned char)s[i];
            bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '-';
            if (!ok) break;
            ++i;
        }
        /* Last char of label cannot be `-`. */
        if (i > mid_start && (unsigned char)s[i - 1] == '-') return false;
        first_label = false;
        (void)first_label;
        if (i >= n) return true;
        if (s[i] != '.') return false;
        ++i;
        if (i >= n) return false;
    }
    return true;
}

static bool emit_autolink(mdit_state_inline *state,
                          mdit_str href,
                          mdit_str text)
{
    mdit_token *open = mdit_state_inline_push(state,
        MDIT_STR_LIT("link_open"), MDIT_STR_LIT("a"), 1);
    if (open == NULL) return false;
    if (!mdit_token_attr_set_z(open, "href", mdit_value_str(href))) return false;
    open->markup = MDIT_STR_LIT("autolink");
    open->info = MDIT_STR_LIT("auto");

    mdit_token *txt = mdit_state_inline_push(state,
        MDIT_STR_LIT("text"), MDIT_STR_LIT(""), 0);
    if (txt == NULL) return false;
    txt->content = text;

    mdit_token *close = mdit_state_inline_push(state,
        MDIT_STR_LIT("link_close"), MDIT_STR_LIT("a"), -1);
    if (close == NULL) return false;
    close->markup = MDIT_STR_LIT("autolink");
    close->info = MDIT_STR_LIT("auto");
    return true;
}

static bool inline_autolink(mdit_state_inline *state, bool silent)
{
    size_t pos = state->pos;
    size_t pos_max = state->pos_max;
    if (pos >= pos_max) return false;
    if (state->src.data[pos] != '<') return false;

    /* Scan until '>' or another '<'. */
    size_t end = pos + 1;
    while (end < pos_max) {
        unsigned char c = (unsigned char)state->src.data[end];
        if (c == '<') return false;
        if (c == '>') break;
        ++end;
    }
    if (end >= pos_max) return false;

    const char *url_start = state->src.data + pos + 1;
    size_t url_len = end - (pos + 1);

    if (autolink_url_ok(url_start, url_len)) {
        mdit_str href;
        if (!mdit_normalize_link(state->arena,
                                 (mdit_str){ url_start, url_len }, &href)) {
            return false;
        }
        if (!mdit_validate_link(href)) return false;
        if (!silent) {
            mdit_str text;
            if (!mdit_normalize_link_text(state->arena,
                                          (mdit_str){ url_start, url_len },
                                          &text)) return false;
            if (!emit_autolink(state, href, text)) return false;
        }
        state->pos += url_len + 2;
        return true;
    }

    if (autolink_email_ok(url_start, url_len)) {
        /* Build "mailto:" + url. */
        size_t total = 7 + url_len;
        char *buf = (char *)mdit_arena_alloc(state->arena, total);
        memcpy(buf, "mailto:", 7);
        memcpy(buf + 7, url_start, url_len);
        mdit_str href;
        if (!mdit_normalize_link(state->arena,
                                 (mdit_str){ buf, total }, &href)) return false;
        if (!mdit_validate_link(href)) return false;
        if (!silent) {
            mdit_str text;
            if (!mdit_normalize_link_text(state->arena,
                                          (mdit_str){ url_start, url_len },
                                          &text)) return false;
            if (!emit_autolink(state, href, text)) return false;
        }
        state->pos += url_len + 2;
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------------
 * Built-in rule: html_inline (`<tag>`, `<!-- ... -->`, etc.).
 *
 * Direct port of rules_inline/html_inline.py. Gated on `options.html`.
 * Emits an `html_inline` token whose content is the raw matched tag;
 * the renderer passes it through verbatim. The `linkLevel` bookkeeping
 * upstream uses to suppress nested links is deferred until the `link`
 * rule lands -- the C state struct doesn't carry that field yet.
 * ------------------------------------------------------------------- */
#include "html_re.h"

static bool html_inline_letter(unsigned char c)
{
    unsigned char lc = (unsigned char)(c | 0x20);
    return lc >= 'a' && lc <= 'z';
}

static bool inline_html(mdit_state_inline *state, bool silent)
{
    size_t pos = state->pos;
    size_t pos_max = state->pos_max;
    if (pos >= pos_max) return false;
    if (!state->md->options.html) return false;
    if (state->src.data[pos] != '<' || pos + 2 >= pos_max) return false;

    unsigned char c1 = (unsigned char)state->src.data[pos + 1];
    if (c1 != '!' && c1 != '?' && c1 != '/' && !html_inline_letter(c1)) {
        return false;
    }

    size_t consumed = mdit_html_match_tag(state->src.data + pos, pos_max - pos);
    if (consumed == 0) return false;

    if (!silent) {
        mdit_token *t = mdit_state_inline_push(state,
            MDIT_STR_LIT("html_inline"), MDIT_STR_LIT(""), 0);
        if (t == NULL) return false;
        char *cbuf = (char *)mdit_arena_alloc(state->arena, consumed);
        memcpy(cbuf, state->src.data + pos, consumed);
        t->content.data = cbuf;
        t->content.len  = consumed;

        /* Track linkify suppression nesting on `<a>` / `</a>`. */
        mdit_str view = (mdit_str){ cbuf, consumed };
        if (mdit_html_is_link_open(view))  ++state->link_level;
        if (mdit_html_is_link_close(view)) --state->link_level;
    }
    state->pos = pos + consumed;
    return true;
}

/* ---------------------------------------------------------------------
 * Built-in rule: linkify (inline trigger on `://`).
 *
 * Direct port of `rules_inline/linkify.py`. Looks at the current
 * `pending` text for a scheme-like prefix, then asks the registered
 * linkifier to validate the rest of the input. Gated on
 * `options.linkify` AND a non-NULL `md->linkifier`; the latter is a
 * pragmatic divergence from upstream Python (which raises an error
 * when linkify is requested without `linkify-it-py` installed).
 * ------------------------------------------------------------------- */
static bool linkify_scheme_in_pending(const char *pending, size_t plen,
                                      size_t *out_proto_len)
{
    /* Mirrors `SCHEME_RE = (?:^|[^a-z0-9.+-])([a-z][a-z0-9.+-]*)$`. */
    if (plen == 0) return false;
    size_t end = plen;
    size_t i = end;
    while (i > 0) {
        unsigned char c = (unsigned char)pending[i - 1];
        bool is_scheme_char =
            ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
             (c >= '0' && c <= '9') || c == '+' || c == '.' || c == '-');
        if (!is_scheme_char) break;
        --i;
    }
    if (i == end) return false;
    /* The first char of the scheme must be a letter. */
    unsigned char first = (unsigned char)pending[i];
    if (!((first >= 'a' && first <= 'z') ||
          (first >= 'A' && first <= 'Z'))) return false;
    /* Boundary on the left: start of pending or non-scheme char. */
    if (i > 0) {
        unsigned char prev = (unsigned char)pending[i - 1];
        if ((prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z') ||
            (prev >= '0' && prev <= '9') || prev == '.' || prev == '+' ||
            prev == '-') return false;
    }
    *out_proto_len = end - i;
    return true;
}

static bool inline_linkify(mdit_state_inline *state, bool silent)
{
    if (!state->md->options.linkify) return false;
    if (state->link_level > 0) return false;
    const mdit_linkifier *L = state->md->linkifier;
    if (L == NULL) return false;

    size_t pos = state->pos;
    size_t pos_max = state->pos_max;
    if (pos + 3 > pos_max) return false;
    if (state->src.data[pos] != ':' ||
        state->src.data[pos + 1] != '/' ||
        state->src.data[pos + 2] != '/') return false;

    size_t proto_len = 0;
    if (!linkify_scheme_in_pending(state->pending.data, state->pending.len,
                                   &proto_len)) return false;
    if (proto_len == 0 || proto_len > pos) return false;

    mdit_str candidate = (mdit_str){
        state->src.data + pos - proto_len,
        pos_max - (pos - proto_len)
    };
    mdit_linkify_match m;
    if (!L->match_at_start(L->self, state->arena, candidate, &m)) {
        return false;
    }

    /* Disallow trailing `*` (conflicts with emphasis). */
    while (m.url.len > 0 && m.url.data[m.url.len - 1] == '*') {
        --m.url.len;
        --m.text.len;
        --m.last_index;
    }
    if (m.url.len == 0) return false;

    mdit_str href;
    if (!mdit_normalize_link(state->arena, m.url, &href)) return false;
    if (!mdit_validate_link(href)) return false;

    if (!silent) {
        /* Strip the scheme bytes that already landed in pending. */
        if (state->pending.len >= proto_len) {
            state->pending.len -= proto_len;
            if (state->pending.data) {
                state->pending.data[state->pending.len] = '\0';
            }
        }

        mdit_token *open = mdit_state_inline_push(state,
            MDIT_STR_LIT("link_open"), MDIT_STR_LIT("a"), 1);
        if (open == NULL) return false;
        if (!mdit_token_attr_set_z(open, "href", mdit_value_str(href))) return false;
        open->markup = MDIT_STR_LIT("linkify");
        open->info   = MDIT_STR_LIT("auto");

        mdit_token *txt = mdit_state_inline_push(state,
            MDIT_STR_LIT("text"), MDIT_STR_LIT(""), 0);
        if (txt == NULL) return false;
        mdit_str rendered;
        if (!mdit_normalize_link_text(state->arena, m.url, &rendered)) {
            return false;
        }
        txt->content = rendered;

        mdit_token *close = mdit_state_inline_push(state,
            MDIT_STR_LIT("link_close"), MDIT_STR_LIT("a"), -1);
        if (close == NULL) return false;
        close->markup = MDIT_STR_LIT("linkify");
        close->info   = MDIT_STR_LIT("auto");
    }

    state->pos += m.url.len - proto_len;
    return true;
}

/* ---------------------------------------------------------------------
 * Built-in rule: emphasis tokenizer (`*`, `_`).
 *
 * The tokenizer only emits marker text tokens and records delimiter
 * metadata. Matching is done by ruler2 `balance_pairs`, and token
 * rewriting by `emphasis_post_process`.
 * ------------------------------------------------------------------- */
typedef struct scanned_delims {
    bool    can_open;
    bool    can_close;
    int32_t length;
} scanned_delims;

static uint32_t prev_codepoint(mdit_str src, size_t pos)
{
    if (pos == 0) return ' ';
    size_t i = 0;
    uint32_t last = ' ';
    while (i < pos) {
        uint32_t cp = ' ';
        size_t n = mdit_decode(src.data + i, pos - i, &cp);
        if (n == 0) break;
        if (i + n > pos) break;
        last = cp;
        i += n;
    }
    return last;
}

static uint32_t next_codepoint(mdit_str src, size_t pos)
{
    if (pos >= src.len) return ' ';
    uint32_t cp = ' ';
    (void)mdit_decode(src.data + pos, src.len - pos, &cp);
    return cp;
}

static scanned_delims scan_delims(mdit_state_inline *state,
                                  size_t start,
                                  bool can_split_word)
{
    scanned_delims out;
    out.can_open = false;
    out.can_close = false;
    out.length = 0;

    char marker = state->src.data[start];
    size_t pos = start;
    while (pos < state->pos_max && state->src.data[pos] == marker) ++pos;
    out.length = (int32_t)(pos - start);

    uint32_t lastChar = prev_codepoint(state->src, start);
    uint32_t nextChar = next_codepoint(state->src, pos);

    bool isLastPunctChar =
        mdit_is_md_ascii_punct(lastChar) || mdit_is_punct(lastChar);
    bool isNextPunctChar =
        mdit_is_md_ascii_punct(nextChar) || mdit_is_punct(nextChar);
    bool isLastWhiteSpace = mdit_is_whitespace(lastChar);
    bool isNextWhiteSpace = mdit_is_whitespace(nextChar);

    bool left_flanking = !(isNextWhiteSpace ||
        (isNextPunctChar && !(isLastWhiteSpace || isLastPunctChar)));
    bool right_flanking = !(isLastWhiteSpace ||
        (isLastPunctChar && !(isNextWhiteSpace || isNextPunctChar)));

    out.can_open = left_flanking &&
        (can_split_word || !right_flanking || isLastPunctChar);
    out.can_close = right_flanking &&
        (can_split_word || !left_flanking || isNextPunctChar);
    return out;
}

static bool inline_emphasis_tokenize(mdit_state_inline *state, bool silent)
{
    size_t start = state->pos;
    if (start >= state->pos_max) return false;
    char marker = state->src.data[start];
    if (marker != '_' && marker != '*') return false;
    if (silent) return false;

    scanned_delims scanned = scan_delims(state, start, marker == '*');
    for (int32_t i = 0; i < scanned.length; ++i) {
        mdit_token *t = mdit_state_inline_push(state,
            MDIT_STR_LIT("text"), MDIT_STR_LIT(""), 0);
        if (t == NULL) return false;
        char *buf = (char *)mdit_arena_alloc(state->arena, 1);
        buf[0] = marker;
        t->content.data = buf;
        t->content.len = 1;

        mdit_delimiter d;
        d.marker = (int32_t)(unsigned char)marker;
        d.length = scanned.length;
        d.token = (int32_t)(state->parent->children_len - 1);
        d.end = -1;
        d.open = scanned.can_open;
        d.close = scanned.can_close;
        (void)mdit_vec_delimiter_push(state->delimiters, d);
    }
    state->pos += (size_t)scanned.length;
    return true;
}

/* ---------------------------------------------------------------------
 * Ruler2: balance_pairs.
 * ------------------------------------------------------------------- */
static void process_delimiters(mdit_state_inline *state,
                               mdit_vec_delimiter *delimiters)
{
    (void)state;
    if (delimiters->len == 0) return;

    int32_t openersBottom[256][6];
    for (size_t i = 0; i < 256; ++i) {
        for (size_t j = 0; j < 6; ++j) openersBottom[i][j] = -1;
    }

    size_t maximum = delimiters->len;
    int32_t *jumps = (int32_t *)mdit_arena_alloc(state->arena,
                                                 maximum * sizeof *jumps);
    for (size_t i = 0; i < maximum; ++i) jumps[i] = 0;

    int32_t headerIdx = 0;
    int32_t lastTokenIdx = -2;

    for (int32_t closerIdx = 0; closerIdx < (int32_t)maximum; ++closerIdx) {
        mdit_delimiter *closer = &delimiters->data[closerIdx];

        if (delimiters->data[headerIdx].marker != closer->marker ||
            lastTokenIdx != closer->token - 1) {
            headerIdx = closerIdx;
        }
        lastTokenIdx = closer->token;

        if (!closer->close) continue;

        int32_t marker = closer->marker & 0xFF;
        int32_t minOpenerIdx =
            openersBottom[marker][(closer->open ? 3 : 0) +
                                  (closer->length % 3)];

        int32_t openerIdx = headerIdx - jumps[headerIdx] - 1;
        int32_t newMinOpenerIdx = openerIdx;

        while (openerIdx > minOpenerIdx) {
            mdit_delimiter *opener = &delimiters->data[openerIdx];

            if (opener->marker != closer->marker) {
                openerIdx -= jumps[openerIdx] + 1;
                continue;
            }

            if (opener->open && opener->end < 0) {
                bool isOddMatch = false;
                if ((opener->close || closer->open) &&
                    ((opener->length + closer->length) % 3 == 0) &&
                    (opener->length % 3 != 0 || closer->length % 3 != 0)) {
                    isOddMatch = true;
                }

                if (!isOddMatch) {
                    int32_t lastJump = 0;
                    if (openerIdx > 0 &&
                        !delimiters->data[openerIdx - 1].open) {
                        lastJump = jumps[openerIdx - 1] + 1;
                    }

                    jumps[closerIdx] = closerIdx - openerIdx + lastJump;
                    jumps[openerIdx] = lastJump;

                    closer->open = false;
                    opener->end = closerIdx;
                    opener->close = false;
                    newMinOpenerIdx = -1;
                    lastTokenIdx = -2;
                    break;
                }
            }
            openerIdx -= jumps[openerIdx] + 1;
        }

        if (newMinOpenerIdx != -1) {
            openersBottom[marker][(closer->open ? 3 : 0) +
                                  (closer->length % 3)] = newMinOpenerIdx;
        }
    }
}

static void ruler2_balance_pairs(mdit_state_inline *state)
{
    process_delimiters(state, state->delimiters);
    /* Each opening token's scoped delimiter list is processed
     * independently. Mirrors upstream `link_pairs`. */
    for (size_t i = 0; i < state->tokens_meta_len; ++i) {
        mdit_vec_delimiter *d = state->tokens_meta[i];
        if (d != NULL) process_delimiters(state, d);
    }
}

/* ---------------------------------------------------------------------
 * Ruler2: emphasis post-process + fragments_join.
 * ------------------------------------------------------------------- */
static void set_token_strong_or_em(mdit_token *t,
                                   bool strong,
                                   bool open,
                                   char marker)
{
    if (strong) {
        t->type = open ? MDIT_STR_LIT("strong_open")
                       : MDIT_STR_LIT("strong_close");
        t->tag = MDIT_STR_LIT("strong");
        t->markup = (marker == '*') ? MDIT_STR_LIT("**")
                                    : MDIT_STR_LIT("__");
    } else {
        t->type = open ? MDIT_STR_LIT("em_open") : MDIT_STR_LIT("em_close");
        t->tag = MDIT_STR_LIT("em");
        t->markup = (marker == '*') ? MDIT_STR_LIT("*") : MDIT_STR_LIT("_");
    }
    t->nesting = open ? 1 : -1;
    t->content = MDIT_STR_LIT("");
}

static void emphasis_post_process_one(mdit_state_inline *state,
                                      mdit_vec_delimiter *delimiters)
{
    if (delimiters->len == 0) return;

    int32_t i = (int32_t)delimiters->len - 1;
    while (i >= 0) {
        mdit_delimiter *startDelim = &delimiters->data[i];
        if (startDelim->marker != '_' && startDelim->marker != '*') {
            --i;
            continue;
        }
        if (startDelim->end == -1) {
            --i;
            continue;
        }

        mdit_delimiter *endDelim = &delimiters->data[startDelim->end];
        bool isStrong =
            i > 0 &&
            delimiters->data[i - 1].end == startDelim->end + 1 &&
            delimiters->data[i - 1].marker == startDelim->marker &&
            delimiters->data[i - 1].token == startDelim->token - 1 &&
            delimiters->data[startDelim->end + 1].token == endDelim->token + 1;

        char marker = (char)startDelim->marker;
        if (startDelim->token >= 0 &&
            (size_t)startDelim->token < state->parent->children_len) {
            set_token_strong_or_em(&state->parent->children[startDelim->token],
                                   isStrong, true, marker);
        }
        if (endDelim->token >= 0 &&
            (size_t)endDelim->token < state->parent->children_len) {
            set_token_strong_or_em(&state->parent->children[endDelim->token],
                                   isStrong, false, marker);
        }

        if (isStrong) {
            if (delimiters->data[i - 1].token >= 0 &&
                (size_t)delimiters->data[i - 1].token <
                    state->parent->children_len) {
                state->parent->children[delimiters->data[i - 1].token].content =
                    MDIT_STR_LIT("");
            }
            if (delimiters->data[startDelim->end + 1].token >= 0 &&
                (size_t)delimiters->data[startDelim->end + 1].token <
                    state->parent->children_len) {
                state->parent
                    ->children[delimiters->data[startDelim->end + 1].token]
                    .content = MDIT_STR_LIT("");
            }
            --i;
        }
        --i;
    }
}

static void ruler2_emphasis_post_process(mdit_state_inline *state)
{
    emphasis_post_process_one(state, state->delimiters);
    for (size_t k = 0; k < state->tokens_meta_len; ++k) {
        mdit_vec_delimiter *d = state->tokens_meta[k];
        if (d != NULL) emphasis_post_process_one(state, d);
    }
}

static void ruler2_fragments_join(mdit_state_inline *state)
{
    mdit_token *tokens = state->parent->children;
    size_t maximum = state->parent->children_len;
    if (tokens == NULL || maximum == 0) return;

    int32_t level = 0;
    size_t curr = 0;
    size_t last = 0;
    while (curr < maximum) {
        if (tokens[curr].nesting < 0) --level;
        tokens[curr].level = level;
        if (tokens[curr].nesting > 0) ++level;

        if (mdit_str_eq_z(tokens[curr].type, "text") &&
            curr + 1 < maximum &&
            mdit_str_eq_z(tokens[curr + 1].type, "text")) {
            size_t start = curr;
            size_t total = 0;
            while (curr < maximum && mdit_str_eq_z(tokens[curr].type, "text")) {
                total += tokens[curr].content.len;
                ++curr;
            }
            mdit_token merged = tokens[curr - 1];
            if (total > 0) {
                char *buf = (char *)mdit_arena_alloc(state->arena, total);
                size_t off = 0;
                for (size_t i = start; i < curr; ++i) {
                    if (tokens[i].content.len > 0) {
                        memcpy(buf + off, tokens[i].content.data,
                               tokens[i].content.len);
                        off += tokens[i].content.len;
                    }
                }
                merged.content.data = buf;
                merged.content.len = total;
            } else {
                merged.content = MDIT_STR_LIT("");
            }
            merged.level = level;
            tokens[last++] = merged;
            continue;
        }

        if (curr != last) tokens[last] = tokens[curr];
        ++last;
        ++curr;
    }
    state->parent->children_len = last;
}

/* ---------------------------------------------------------------------
 * Built-in rule: newline ('\n' inside inline content).
 *
 * Two variants: trailing "  \n" yields a hardbreak (`<br>`), any other
 * '\n' yields a softbreak (`<br>`-with-newline). Trailing spaces in
 * the pending buffer are stripped, and leading spaces on the next line
 * are skipped after the newline.
 * ------------------------------------------------------------------- */
static bool inline_newline(mdit_state_inline *state, bool silent)
{
    if (state->pos >= state->pos_max) return false;
    if (state->src.data[state->pos] != '\n') return false;

    if (!silent) {
        size_t plen = state->pending.len;
        const char *pdata = state->pending.data;
        if (plen >= 1 && pdata[plen - 1] == ' ') {
            if (plen >= 2 && pdata[plen - 2] == ' ') {
                /* Hard break: walk back to strip *all* trailing spaces. */
                size_t ws = plen - 1;
                while (ws >= 1 && pdata[ws - 1] == ' ') --ws;
                state->pending.len = ws;
                if (state->pending.data) state->pending.data[ws] = '\0';
                (void)mdit_state_inline_push(state,
                    MDIT_STR_LIT("hardbreak"), MDIT_STR_LIT("br"), 0);
            } else {
                /* Soft break: drop the single trailing space. */
                state->pending.len = plen - 1;
                if (state->pending.data) state->pending.data[plen - 1] = '\0';
                (void)mdit_state_inline_push(state,
                    MDIT_STR_LIT("softbreak"), MDIT_STR_LIT("br"), 0);
            }
        } else {
            (void)mdit_state_inline_push(state,
                MDIT_STR_LIT("softbreak"), MDIT_STR_LIT("br"), 0);
        }
    }

    size_t pos = state->pos + 1;
    while (pos < state->pos_max &&
           ascii_is_space((unsigned char)state->src.data[pos])) ++pos;
    state->pos = pos;
    return true;
}

/* ---------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------- */
static void terminator_init_default(bool *t)
{
    memset(t, 0, sizeof(bool) * 256);
    for (const char *p = DEFAULT_TERMINATORS; *p; ++p) {
        t[(unsigned char)*p] = true;
    }
}

/* Forward declarations for rules defined further down. */
static bool inline_link (mdit_state_inline *state, bool silent);
static bool inline_image(mdit_state_inline *state, bool silent);

bool mdit_parser_inline_init(mdit_parser_inline *p, mdit_arena *arena)
{
    p->arena  = arena;
    p->ruler  = mdit_ruler_new(arena);
    p->ruler2 = mdit_ruler_new(arena);
    if (p->ruler == NULL || p->ruler2 == NULL) return false;
    terminator_init_default(p->terminator_ascii);
    /* Order matches upstream parser_inline.py. text is the catch-all. */
    if (mdit_ruler_push(p->ruler, MDIT_STR_LIT("text"),
                        (mdit_rule_fn)inline_text, NULL,
                        MDIT_RULE_OPTIONS_NONE).index < 0) return false;
    if (mdit_ruler_push(p->ruler, MDIT_STR_LIT("linkify"),
                        (mdit_rule_fn)inline_linkify, NULL,
                        MDIT_RULE_OPTIONS_NONE).index < 0) return false;
    if (mdit_ruler_push(p->ruler, MDIT_STR_LIT("newline"),
                        (mdit_rule_fn)inline_newline, NULL,
                        MDIT_RULE_OPTIONS_NONE).index < 0) return false;
    if (mdit_ruler_push(p->ruler, MDIT_STR_LIT("escape"),
                        (mdit_rule_fn)inline_escape, NULL,
                        MDIT_RULE_OPTIONS_NONE).index < 0) return false;
    if (mdit_ruler_push(p->ruler, MDIT_STR_LIT("backticks"),
                        (mdit_rule_fn)inline_backticks, NULL,
                        MDIT_RULE_OPTIONS_NONE).index < 0) return false;
    if (mdit_ruler_push(p->ruler, MDIT_STR_LIT("emphasis"),
                        (mdit_rule_fn)inline_emphasis_tokenize, NULL,
                        MDIT_RULE_OPTIONS_NONE).index < 0) return false;
    if (mdit_ruler_push(p->ruler, MDIT_STR_LIT("link"),
                        (mdit_rule_fn)inline_link, NULL,
                        MDIT_RULE_OPTIONS_NONE).index < 0) return false;
    if (mdit_ruler_push(p->ruler, MDIT_STR_LIT("image"),
                        (mdit_rule_fn)inline_image, NULL,
                        MDIT_RULE_OPTIONS_NONE).index < 0) return false;
    if (mdit_ruler_push(p->ruler, MDIT_STR_LIT("autolink"),
                        (mdit_rule_fn)inline_autolink, NULL,
                        MDIT_RULE_OPTIONS_NONE).index < 0) return false;
    if (mdit_ruler_push(p->ruler, MDIT_STR_LIT("html_inline"),
                        (mdit_rule_fn)inline_html, NULL,
                        MDIT_RULE_OPTIONS_NONE).index < 0) return false;
    if (mdit_ruler_push(p->ruler, MDIT_STR_LIT("entity"),
                        (mdit_rule_fn)inline_entity, NULL,
                        MDIT_RULE_OPTIONS_NONE).index < 0) return false;
    if (mdit_ruler_push(p->ruler2, MDIT_STR_LIT("balance_pairs"),
                        (mdit_rule_fn)ruler2_balance_pairs, NULL,
                        MDIT_RULE_OPTIONS_NONE).index < 0) return false;
    if (mdit_ruler_push(p->ruler2, MDIT_STR_LIT("emphasis"),
                        (mdit_rule_fn)ruler2_emphasis_post_process, NULL,
                        MDIT_RULE_OPTIONS_NONE).index < 0) return false;
    if (mdit_ruler_push(p->ruler2, MDIT_STR_LIT("fragments_join"),
                        (mdit_rule_fn)ruler2_fragments_join, NULL,
                        MDIT_RULE_OPTIONS_NONE).index < 0) return false;
    return true;
}

void mdit_parser_inline_destroy(mdit_parser_inline *p)
{
    mdit_ruler_destroy(p->ruler);
    mdit_ruler_destroy(p->ruler2);
    p->ruler = NULL;
    p->ruler2 = NULL;
}

void mdit_parser_inline_add_terminator(mdit_parser_inline *p, char ch)
{
    p->terminator_ascii[(unsigned char)ch] = true;
}

/* ---------------------------------------------------------------------
 * Built-in rules: link, image
 *
 * Direct ports of `rules_inline/link.py` and `rules_inline/image.py`.
 * Both share the same destination/title scanning helpers and rely on
 * `parse_link_label` to find the closing `]`. References are looked
 * up against `state->env` (a `mdit_env *`).
 * ------------------------------------------------------------------- */
static bool li_is_str_space(unsigned char c)
{
    return c == ' ' || c == '\t';
}

static size_t li_skip_ws_inc_lf(mdit_str src, size_t pos, size_t maximum)
{
    while (pos < maximum) {
        unsigned char ch = (unsigned char)src.data[pos];
        if (!li_is_str_space(ch) && ch != '\n') break;
        ++pos;
    }
    return pos;
}

/* Re-tokenize an inner range into the *current* parent.
 *
 * Mirrors upstream's `state.md.inline.tokenize(state)` call inside
 * `link`. The state's pos / pos_max are mutated by the caller before
 * the call and restored after. */
static void link_tokenize_inner(mdit_parser_inline *p,
                                mdit_state_inline *state)
{
    /* See the static `tokenize` function further down: this is the
     * same loop, minus the trailing pending flush (which the outer
     * tokenize will perform after `link_close` is pushed). */
    int32_t maxNesting = (int32_t)state->md->options.max_nesting;
    size_t n_rules = 0;
    const mdit_rule_entry *rules =
        mdit_ruler_get_rules(p->ruler, MDIT_STR_LIT(""), &n_rules);
    while (state->pos < state->pos_max) {
        bool ok = false;
        if (state->level < maxNesting) {
            for (size_t i = 0; i < n_rules; ++i) {
                mdit_inline_rule_fn fn = (mdit_inline_rule_fn)rules[i].fn;
                ok = fn(state, false);
                if (ok) break;
            }
        }
        if (ok) {
            if (state->pos >= state->pos_max) break;
            continue;
        }
        (void)mdit_state_inline_append_pending(state,
            state->src.data + state->pos, 1);
        ++state->pos;
    }
}

static bool inline_link(mdit_state_inline *state, bool silent)
{
    size_t pos     = state->pos;
    size_t maximum = state->pos_max;
    mdit_str src   = state->src;
    if (pos >= maximum || src.data[pos] != '[') return false;

    mdit_parser_inline *p = &state->md->inline_p;
    size_t old_pos = pos;
    size_t label_start = pos + 1;
    ptrdiff_t label_end_s = mdit_parse_link_label(p, state, pos, true);
    if (label_end_s < 0) return false;
    size_t label_end = (size_t)label_end_s;

    size_t cursor = label_end + 1;
    bool parse_reference = true;
    mdit_str href = MDIT_STR_LIT("");
    mdit_str title = MDIT_STR_LIT("");
    mdit_str label = { NULL, 0 };

    if (cursor < maximum && src.data[cursor] == '(') {
        parse_reference = false;
        ++cursor;
        cursor = li_skip_ws_inc_lf(src, cursor, maximum);
        if (cursor >= maximum) return false;

        size_t dest_start = cursor;
        mdit_link_destination_result dr;
        if (!mdit_parse_link_destination(state->arena, src, cursor, maximum, &dr)) {
            return false;
        }
        if (dr.ok) {
            mdit_str norm;
            if (mdit_normalize_link(state->arena, dr.str, &norm) &&
                mdit_validate_link(norm)) {
                href = norm;
                cursor = dr.pos;
            } else {
                href = MDIT_STR_LIT("");
            }

            size_t after_dest = cursor;
            cursor = li_skip_ws_inc_lf(src, cursor, maximum);

            mdit_link_title_result tr;
            if (mdit_parse_link_title(state->arena, src,
                                      cursor, maximum, NULL, &tr) &&
                cursor < maximum && after_dest != cursor && tr.ok) {
                title = tr.str;
                cursor = tr.pos;
                cursor = li_skip_ws_inc_lf(src, cursor, maximum);
            }
            (void)dest_start;
        }

        if (cursor >= maximum || src.data[cursor] != ')') {
            parse_reference = true;
        }
        ++cursor;
    }

    if (parse_reference) {
        mdit_env *env = (mdit_env *)state->env;
        if (env == NULL) return false;

        bool used_explicit_label = false;
        if (cursor < maximum && src.data[cursor] == '[') {
            size_t lstart = cursor + 1;
            ptrdiff_t lr = mdit_parse_link_label(p, state, cursor, false);
            if (lr >= 0) {
                label = (mdit_str){ src.data + lstart, (size_t)lr - lstart };
                cursor = (size_t)lr + 1;
                used_explicit_label = true;
            } else {
                cursor = label_end + 1;
            }
        } else {
            cursor = label_end + 1;
        }

        if (label.data == NULL || label.len == 0) {
            label = (mdit_str){ src.data + label_start, label_end - label_start };
        }
        (void)used_explicit_label;

        mdit_str norm = mdit_env_normalize_reference(state->arena, label);
        const mdit_reference *ref = mdit_env_get_reference(env, norm);
        if (ref == NULL) {
            state->pos = old_pos;
            return false;
        }
        href = ref->href;
        title = ref->title;
    }

    if (!silent) {
        state->pos     = label_start;
        state->pos_max = label_end;

        mdit_token *open = mdit_state_inline_push(state,
            MDIT_STR_LIT("link_open"), MDIT_STR_LIT("a"), 1);
        if (open == NULL) return false;
        if (!mdit_token_attr_set_z(open, "href", mdit_value_str(href))) return false;
        if (title.len > 0) {
            if (!mdit_token_attr_set_z(open, "title", mdit_value_str(title))) {
                return false;
            }
        }

        ++state->link_level;
        link_tokenize_inner(p, state);
        --state->link_level;

        (void)mdit_state_inline_push(state,
            MDIT_STR_LIT("link_close"), MDIT_STR_LIT("a"), -1);
    }

    state->pos     = cursor;
    state->pos_max = maximum;
    return true;
}

static bool inline_image(mdit_state_inline *state, bool silent)
{
    size_t pos     = state->pos;
    size_t maximum = state->pos_max;
    mdit_str src   = state->src;
    if (pos >= maximum || src.data[pos] != '!') return false;
    if (pos + 1 >= maximum || src.data[pos + 1] != '[') return false;

    mdit_parser_inline *p = &state->md->inline_p;
    size_t old_pos = pos;
    size_t label_start = pos + 2;
    ptrdiff_t label_end_s = mdit_parse_link_label(p, state, pos + 1, false);
    if (label_end_s < 0) return false;
    size_t label_end = (size_t)label_end_s;

    size_t cursor = label_end + 1;
    mdit_str href  = MDIT_STR_LIT("");
    mdit_str title = MDIT_STR_LIT("");
    mdit_str label = { NULL, 0 };

    if (cursor < maximum && src.data[cursor] == '(') {
        ++cursor;
        cursor = li_skip_ws_inc_lf(src, cursor, maximum);
        if (cursor >= maximum) return false;

        mdit_link_destination_result dr;
        if (!mdit_parse_link_destination(state->arena, src, cursor, maximum, &dr)) {
            return false;
        }
        if (dr.ok) {
            mdit_str norm;
            if (mdit_normalize_link(state->arena, dr.str, &norm) &&
                mdit_validate_link(norm)) {
                href = norm;
                cursor = dr.pos;
            } else {
                href = MDIT_STR_LIT("");
            }
        }
        size_t after_dest = cursor;
        cursor = li_skip_ws_inc_lf(src, cursor, maximum);

        mdit_link_title_result tr;
        if (mdit_parse_link_title(state->arena, src,
                                  cursor, maximum, NULL, &tr) &&
            cursor < maximum && after_dest != cursor && tr.ok) {
            title = tr.str;
            cursor = tr.pos;
            cursor = li_skip_ws_inc_lf(src, cursor, maximum);
        }

        if (cursor >= maximum || src.data[cursor] != ')') {
            state->pos = old_pos;
            return false;
        }
        ++cursor;
    } else {
        mdit_env *env = (mdit_env *)state->env;
        if (env == NULL) return false;

        if (cursor < maximum && src.data[cursor] == '[') {
            size_t lstart = cursor + 1;
            ptrdiff_t lr = mdit_parse_link_label(p, state, cursor, false);
            if (lr >= 0) {
                label = (mdit_str){ src.data + lstart, (size_t)lr - lstart };
                cursor = (size_t)lr + 1;
            } else {
                cursor = label_end + 1;
            }
        } else {
            cursor = label_end + 1;
        }

        if (label.data == NULL || label.len == 0) {
            label = (mdit_str){ src.data + label_start, label_end - label_start };
        }

        mdit_str norm = mdit_env_normalize_reference(state->arena, label);
        const mdit_reference *ref = mdit_env_get_reference(env, norm);
        if (ref == NULL) {
            state->pos = old_pos;
            return false;
        }
        href = ref->href;
        title = ref->title;
    }

    if (!silent) {
        mdit_str content = (mdit_str){ src.data + label_start,
                                       label_end - label_start };

        /* Parse content as a fully-isolated inline tree. */
        mdit_token *fake_parent = mdit_token_new(state->arena,
            MDIT_STR_LIT(""), MDIT_STR_LIT(""), 0);
        if (fake_parent == NULL) return false;
        if (!mdit_parser_inline_parse(p, content, state->md, state->env,
                                      fake_parent)) {
            return false;
        }

        mdit_token *t = mdit_state_inline_push(state,
            MDIT_STR_LIT("image"), MDIT_STR_LIT("img"), 0);
        if (t == NULL) return false;
        if (!mdit_token_attr_set_z(t, "src", mdit_value_str(href))) return false;
        if (!mdit_token_attr_set_z(t, "alt", mdit_value_str(MDIT_STR_LIT("")))) {
            return false;
        }
        if (title.len > 0) {
            if (!mdit_token_attr_set_z(t, "title", mdit_value_str(title))) {
                return false;
            }
        }
        t->children     = fake_parent->children;
        t->children_len = fake_parent->children_len;
        t->children_cap = fake_parent->children_cap;
        t->content      = content;
    }

    state->pos     = cursor;
    state->pos_max = maximum;
    return true;
}

/* ---------------------------------------------------------------------
 * Tokenize + post-process
 * ------------------------------------------------------------------- */
static void tokenize(mdit_parser_inline *p, mdit_state_inline *state)
{
    size_t n_rules = 0;
    const mdit_rule_entry *rules =
        mdit_ruler_get_rules(p->ruler, MDIT_STR_LIT(""), &n_rules);
    int32_t maxNesting = (int32_t)state->md->options.max_nesting;
    while (state->pos < state->pos_max) {
        bool ok = false;
        if (state->level < maxNesting) {
            for (size_t i = 0; i < n_rules; ++i) {
                mdit_inline_rule_fn fn = (mdit_inline_rule_fn)rules[i].fn;
                ok = fn(state, false);
                if (ok) break;
            }
        }
        if (ok) {
            if (state->pos >= state->pos_max) break;
            continue;
        }
        /* No rule matched: append the byte at state.src[state.pos] to
         * pending and advance. */
        (void)mdit_state_inline_append_pending(state,
            state->src.data + state->pos, 1);
        ++state->pos;
    }
    if (state->pending.len > 0) {
        (void)mdit_state_inline_push_pending(state);
    }
}

void mdit_parser_inline_skip_token(mdit_parser_inline *p,
                                   mdit_state_inline *state)
{
    size_t pos = state->pos;
    /* Lazily allocate cache: src.len + 1 entries, all -1. */
    if (state->cache == NULL && state->src.len > 0) {
        size_t n = state->src.len + 1;
        int32_t *buf = (int32_t *)malloc(n * sizeof *buf);
        if (buf != NULL) {
            for (size_t i = 0; i < n; ++i) buf[i] = -1;
            state->cache     = buf;
            state->cache_len = n;
        }
    }
    if (state->cache != NULL && pos < state->cache_len &&
        state->cache[pos] >= 0) {
        state->pos = (size_t)state->cache[pos];
        return;
    }

    int32_t maxNesting = (int32_t)state->md->options.max_nesting;
    bool ok = false;
    if (state->level < maxNesting) {
        size_t n_rules = 0;
        const mdit_rule_entry *rules =
            mdit_ruler_get_rules(p->ruler, MDIT_STR_LIT(""), &n_rules);
        for (size_t i = 0; i < n_rules; ++i) {
            mdit_inline_rule_fn fn = (mdit_inline_rule_fn)rules[i].fn;
            ++state->level;
            ok = fn(state, true);
            --state->level;
            if (ok) break;
        }
    } else {
        /* Too much nesting: skip to end of paragraph. */
        state->pos = state->pos_max;
    }
    if (!ok) ++state->pos;

    if (state->cache != NULL && pos < state->cache_len) {
        state->cache[pos] = (int32_t)state->pos;
    }
}

bool mdit_parser_inline_parse(mdit_parser_inline *p,
                              mdit_str src,
                              struct mdit_md *md,
                              void *env,
                              mdit_token *parent)
{
    mdit_state_inline state;
    mdit_state_inline_init(&state, p->arena, src, md, env, parent);
    tokenize(p, &state);

    /* Run ruler2 (post-processing). */
    size_t n2 = 0;
    const mdit_rule_entry *rules2 =
        mdit_ruler_get_rules(p->ruler2, MDIT_STR_LIT(""), &n2);
    for (size_t i = 0; i < n2; ++i) {
        mdit_inline_rule2_fn fn = (mdit_inline_rule2_fn)rules2[i].fn;
        fn(&state);
    }
    mdit_state_inline_destroy(&state);
    return true;
}
