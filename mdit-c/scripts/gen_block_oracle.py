"""Generate `mdit-c/tests/block_oracle.h` — a small oracle of
`(markdown_input, expected_html)` pairs covering every block rule the
C port has shipped so far.

The oracle is pinned in CI: the workflow regenerates it and fails on
drift, so the C tests can never silently diverge from upstream Python.

Each row carries:

    title    — short label (used by the C runner when printing failures)
    input    — markdown source bytes (verbatim, including \r and \0 if any)
    html     — RendererHTML output for `MarkdownIt('default')`

`MarkdownIt('default')` matches the C port's defaults
(`xhtmlOut=False`, `maxNesting=100`, no linkify / typographer); using
`commonmark` here would diverge on `<hr />` vs `<hr>` etc.
"""
from __future__ import annotations

from pathlib import Path

from markdown_it import MarkdownIt

OUT = Path(__file__).resolve().parents[1] / "tests" / "block_oracle.h"


# Each case is `(title, input_md)` or `(title, input_md, options)`.
# `options` is a dict of upstream MarkdownIt options to enable for this
# case (e.g. `{"html": True}` for html_block / html_inline). When
# omitted, defaults are used; the C runner mirrors that by initialising
# `mdit_md` with `MDIT_OPTIONS_DEFAULTS` and applying the same overrides.
CASES: list = [
    # paragraph baseline (covered by Phase 2a but kept here for the
    # oracle to be self-contained)
    ("paragraph_simple",        "hello"),
    ("paragraph_multiline",     "a\nb"),
    ("paragraph_two",           "foo\n\nbar"),

    # ATX headings
    ("heading_h1",              "# title"),
    ("heading_h2",              "## title"),
    ("heading_h6",              "###### title"),
    ("heading_h7_invalid",      "####### too deep"),
    ("heading_close_hashes",    "## title ##"),
    ("heading_trailing_hashes_no_space", "## title##"),
    ("heading_empty",           "#"),
    ("heading_empty_with_space","#   "),
    ("heading_no_space_after_hash", "#bad"),
    ("heading_then_paragraph",  "# title\n\nparagraph"),

    # Setext headings
    ("lheading_eq",             "title\n==="),
    ("lheading_dash",           "title\n---"),
    ("lheading_multiline",      "first\nsecond\n==="),
    ("lheading_then_para",      "title\n===\n\npara"),

    # HR
    ("hr_dash",                 "---"),
    ("hr_star",                 "***"),
    ("hr_underscore",           "___"),
    ("hr_too_few",              "--"),
    ("hr_with_spaces",          " - - -"),
    ("hr_long",                 "-----"),
    ("hr_then_para",            "---\nfoo"),

    # Indented code
    ("code_simple",             "    line1\n    line2"),
    ("code_with_blank_line",    "    a\n\n    b"),

    # Fenced code
    ("fence_backtick",          "```\nhello\n```"),
    ("fence_lang",              "```python\nprint(1)\n```"),
    ("fence_tilde",             "~~~\nplain\n~~~"),
    ("fence_unclosed",          "```\nno end"),
    ("fence_closing_longer_ok", "```\nbody\n````"),
    ("fence_inner_backticks_in_lang", "``` foo`bar\n"),
    ("fence_indented_2sp",      "  ```\n  body\n  ```"),

    # Blockquotes
    ("bq_simple",               "> text"),
    ("bq_multiline",            "> a\n> b"),
    ("bq_with_para_continuation","> a\nb"),
    ("bq_nested",               "> > deep"),
    ("bq_nested_3deep",         "> > > triple"),
    ("bq_nested_with_para",     "> > a\n> > b\n>\n> c"),
    ("bq_with_heading",         "> # title"),
    ("bq_with_fence",           "> ```\n> code\n> ```"),
    ("bq_with_hr",              "> ---"),
    ("bq_with_lheading",        "> title\n> ==="),
    ("bq_followed_by_para",     "> a\n\nb"),
    ("bq_followed_by_hr",       "> a\n\n---"),
    ("bq_blank_inside",         "> a\n>\n> b"),
    ("bq_with_indented_marker", "  > text"),
    ("bq_lazy_continuation",    "> a\nb\n> c"),

    # Bullet lists
    ("ul_dash_single",          "- item"),
    ("ul_star_single",           "* item"),
    ("ul_plus_single",           "+ item"),
    ("ul_dash_two_items",        "- a\n- b"),
    ("ul_star_two_items",        "* a\n* b"),
    ("ul_loose",                 "- a\n\n- b"),
    ("ul_with_continuation",     "- a\n  b"),
    ("ul_nested",                "- a\n  - b"),
    ("ul_nested_3deep",          "- a\n  - b\n    - c"),
    ("ul_followed_by_para",      "- a\n\nfoo"),
    ("ul_terminate_paragraph",   "para\n- a"),
    ("ul_with_heading",          "- # h"),
    ("ul_with_blockquote",       "- > q"),

    # Ordered lists
    ("ol_simple",                "1. a\n2. b"),
    ("ol_paren",                 "1) a\n2) b"),
    ("ol_start_5",               "5. a\n6. b"),
    ("ol_terminate_must_start_1","para\n2. a"),
    ("ol_loose",                 "1. a\n\n2. b"),
    ("ol_continuation",          "1. a\n   b"),
    ("ol_nested",                "1. a\n   1. b"),
    ("ol_long_marker",           "12345. a"),
    ("ol_too_many_digits",       "1234567890. a"),

    # Lists mixed with other rules
    ("list_then_hr",             "- a\n\n---"),
    ("list_with_blank_lines",    "- a\n  \n- b"),
    ("list_followed_by_fence",   "- a\n\n```\ncode\n```"),

    # Mixed scenarios that exercise rule ordering
    ("mixed_para_hr_para",      "foo\n\n---\n\nbar"),
    ("mixed_heading_then_hr",   "# h\n\n---"),
    ("mixed_hr_then_heading",   "---\n\n## sub"),

    # Inline rule: escape (\X)
    ("inline_escape_punct",          "\\!\\\"\\#"),
    ("inline_escape_hash",           "\\# not a heading"),
    ("inline_escape_in_para",        "a \\* b"),
    ("inline_escape_unknown",        "\\a"),
    ("inline_escape_double",         "\\\\"),
    ("inline_escape_at_eol",         "foo\\\nbar"),
    ("inline_escape_backslash_eof",  "foo\\"),
    ("inline_escape_inside_heading", "# title \\#"),

    # Inline rule: backticks (`code`)
    ("inline_code_simple",           "`x`"),
    ("inline_code_double",           "`` `y` ``"),
    ("inline_code_strip_spaces",     "` foo `"),
    ("inline_code_no_strip_one",     "` `"),
    ("inline_code_unmatched",        "`foo"),
    ("inline_code_unmatched_double", "``a`b"),
    ("inline_code_in_heading",       "# h `c`"),
    ("inline_code_in_blockquote",    "> q `c`"),
    ("inline_code_in_list",          "- l `c`"),
    ("inline_code_with_newline",     "`a\nb`"),
    ("inline_code_only_spaces",      "`   `"),

    # Inline rule: entity (&...; / &#...; / &#x...;)
    ("inline_entity_named",          "&amp;"),
    ("inline_entity_unknown",        "&abcdef;"),
    ("inline_entity_decimal",        "&#65;"),
    ("inline_entity_hex_lower",      "&#x41;"),
    ("inline_entity_hex_upper",      "&#X41;"),
    ("inline_entity_invalid_code",   "&#0;"),
    ("inline_entity_too_many_digits","&#12345678;"),
    ("inline_entity_lone_amp",       "& not entity"),
    ("inline_entity_in_heading",     "# &amp;"),
    ("inline_entity_in_para",        "a &amp; b"),

    # Mixes of inline rules
    ("inline_escape_then_code",      "\\* `x`"),
    ("inline_entity_then_code",      "&amp; `x`"),
    ("inline_code_with_entity",      "`&amp;`"),
    ("inline_escape_in_code",        "`a\\b`"),

    # Block rule: reference definitions (no output, but consumes lines)
    ("reference_simple",             "[foo]: /url"),
    ("reference_with_title_double",  "[foo]: /url \"title\""),
    ("reference_with_title_single",  "[foo]: /url 'title'"),
    ("reference_with_title_parens",  "[foo]: /url (title)"),
    ("reference_angle_dest",         "[foo]: <a b>"),
    ("reference_escaped_dest",       "[foo]: /a\\ b"),
    ("reference_escaped_title",      "[foo]: /url \"a \\\" b\""),
    ("reference_multiline_title",    "[foo]: /url \"a\nb\""),
    ("reference_label_casefold",     "[Foo Bar]: /url"),
    ("reference_duplicate",          "[foo]: /a\n[foo]: /b"),
    ("reference_then_para",          "[foo]: /url\n\ntext"),
    ("reference_terminates_para",    "a\n[foo]: /url"),
    ("reference_invalid_empty_label", "[]: /url"),
    ("reference_invalid_no_dest",    "[foo]:"),
    ("reference_invalid_bad_title",  "[foo]: /url \"title\" garbage"),
    ("reference_invalid_nested_label","[foo[bar]]: /url"),
    ("reference_reject_javascript",  "[foo]: javascript:alert(1)"),
    ("reference_continued_label",    "[foo\nbar]: /url"),
    ("reference_continued_dest",     "[foo]: <a\nb>"),

    # Block rule: html_block (gated on `html=True`)
    ("html_block_div",                "<div>\nhello\n</div>",                          {"html": True}),
    ("html_block_div_close_only",     "<div>\n</div>",                                 {"html": True}),
    ("html_block_div_with_attrs",     "<div class=\"x\" id='y'>\nhi\n</div>",         {"html": True}),
    ("html_block_self_closing",       "<hr />",                                        {"html": True}),
    ("html_block_p_inline",           "<p>line</p>",                                   {"html": True}),
    ("html_block_p_then_para",        "<p>line</p>\n\nafter",                          {"html": True}),
    ("html_block_p_termination",      "before\n<p>line</p>",                           {"html": True}),
    ("html_block_disabled_default",   "<div>\nhi\n</div>"),
    ("html_block_seq1_script",        "<script>\nalert(1)\n</script>",                 {"html": True}),
    ("html_block_seq1_pre",           "<pre>\ntext\n</pre>",                           {"html": True}),
    ("html_block_seq1_style",         "<style>\n.a{}\n</style>",                       {"html": True}),
    ("html_block_seq1_textarea",      "<textarea>\nabc\n</textarea>",                  {"html": True}),
    ("html_block_seq1_inline_close",  "<style>x</style>\nfoo",                         {"html": True}),
    ("html_block_seq2_comment",       "<!-- a\nb -->",                                 {"html": True}),
    ("html_block_seq2_comment_short", "<!-- abc -->",                                  {"html": True}),
    ("html_block_seq3_pi",            "<?php\necho 1;\n?>",                            {"html": True}),
    ("html_block_seq4_decl",          "<!DOCTYPE html>",                               {"html": True}),
    ("html_block_seq4_decl_multi",    "<!DOCTYPE html\nlang=\"x\">",                   {"html": True}),
    ("html_block_seq5_cdata",         "<![CDATA[\nfoo\n]]>",                           {"html": True}),
    ("html_block_seq6_block_name",    "<table>\n<tr><td>x</td></tr>\n</table>",        {"html": True}),
    ("html_block_seq6_h1",            "<h1>top</h1>",                                  {"html": True}),
    ("html_block_seq6_close",         "</p>",                                          {"html": True}),
    ("html_block_seq7_open",          "<a href=\"x\">",                                {"html": True}),
    ("html_block_seq7_no_paragraph_term", "before\n<a href=\"x\">",                    {"html": True}),
    ("html_block_in_blockquote",      "> <div>\n> hi\n> </div>",                       {"html": True}),
    ("html_block_unknown_tag",        "<unknowntag>\nhi\n</unknowntag>",               {"html": True}),
    ("html_block_lt_then_text",       "<x ",                                           {"html": True}),

    # Block rule: table (GFM)
    ("table_simple",                  "| a | b |\n|---|---|\n| 1 | 2 |"),
    ("table_no_outer_pipes",          "a | b\n--- | ---\n1 | 2"),
    ("table_single_column",           "| a |\n|---|\n| x |"),
    ("table_align_left",              "| a | b |\n|:--|:--|\n| 1 | 2 |"),
    ("table_align_right",             "| a | b |\n|--:|--:|\n| 1 | 2 |"),
    ("table_align_center",            "| a | b |\n|:-:|:-:|\n| 1 | 2 |"),
    ("table_align_mixed",             "| a | b | c |\n|:--|:-:|--:|\n| 1 | 2 | 3 |"),
    ("table_no_body",                 "| a | b |\n|---|---|"),
    ("table_multibody",               "| a | b |\n|---|---|\n| 1 | 2 |\n| 3 | 4 |"),
    ("table_short_row_autocomplete",  "| a | b |\n|---|---|\n| 1 |"),
    ("table_extra_cells_dropped",     "| a | b |\n|---|---|\n| 1 | 2 | 3 |"),
    ("table_escaped_pipe_in_cell",    "| a | b |\n|---|---|\n| x \\| y | z |"),
    ("table_terminated_by_blank",     "| a | b |\n|---|---|\n| 1 | 2 |\n\nafter"),
    ("table_terminated_by_heading",   "| a | b |\n|---|---|\n| 1 | 2 |\n# next"),
    ("table_terminated_by_hr",        "| a | b |\n|---|---|\n| 1 | 2 |\n---"),
    ("table_after_paragraph",         "para\n| a | b |\n|---|---|\n| 1 | 2 |"),
    ("table_inside_blockquote",       "> | a | b |\n> |---|---|\n> | 1 | 2 |"),
    ("table_with_inline_code",        "| a | b |\n|---|---|\n| `x` | y |"),
    ("table_with_escape",             "| a | b |\n|---|---|\n| \\* | y |"),
    ("table_invalid_no_divider",      "| a | b |\n| 1 | 2 |"),
    ("table_invalid_only_one_line",   "| a | b |"),
    ("table_invalid_empty_middle",    "| a | b |\n| --- || --- |"),
    ("table_invalid_dash_then_space", "| a |\n- not a divider"),
    ("table_invalid_count_mismatch",  "| a | b | c |\n|---|---|"),
    ("table_no_pipe_in_header",       "abc\n---|---"),

    # Inline rule: emphasis + balance_pairs + fragments_join
    ("emphasis_star",                 "*a*"),
    ("emphasis_underscore",           "_a_"),
    ("emphasis_strong_star",          "**a**"),
    ("emphasis_strong_underscore",    "__a__"),
    ("emphasis_nested",               "**a *b* c**"),
    ("emphasis_nested_reverse",       "*a **b** c*"),
    ("emphasis_triple_star",          "***a***"),
    ("emphasis_triple_underscore",    "___a___"),
    ("emphasis_unmatched_open",       "*a"),
    ("emphasis_unmatched_close",      "a*"),
    ("emphasis_literal_middle_word",  "foo_bar_baz"),
    ("emphasis_star_middle_word",     "foo*bar*baz"),
    ("emphasis_punct_flanking",       "a*\"b\"*c"),
    ("emphasis_rule_of_three",        "*a**b*"),
    ("emphasis_many_markers",         "****a****"),
    ("emphasis_with_escape",          "\\*a*"),
    ("emphasis_with_entity",          "*a &amp; b*"),
    ("emphasis_with_code",            "*a `*` b*"),
    ("emphasis_in_heading",           "# *a* **b**"),
    ("emphasis_in_blockquote",        "> *a*"),
    ("emphasis_in_list",              "- *a*"),
    ("emphasis_in_table",             "| a | b |\n|---|---|\n| *x* | **y** |"),

    # Inline rule: strikethrough (~~text~~), GFM extension. Enabled by
    # default in `MarkdownIt('default')`; matches CommonMark commonmark
    # preset's *exclusion* via the explicit `disabled` path on the C
    # side (see `cli/md_cli.c`).
    ("strikethrough_basic",           "~~strike~~"),
    ("strikethrough_in_paragraph",    "before ~~mid~~ after"),
    ("strikethrough_with_emph",       "~~bold *italic* end~~"),
    ("strikethrough_nested",
     "outer ~~one ~~two~~ three~~ end"),
    ("strikethrough_unmatched",       "~~unclosed strike"),
    ("strikethrough_single_tilde_off","one ~tilde~ between"),
    ("strikethrough_three_tildes",    "~~~ literal ~~~"),
    ("strikethrough_four_tildes",     "~~~~ four pair"),
    ("strikethrough_with_link",
     "~~[link](http://x.example)~~"),
    ("strikethrough_in_heading",      "# ~~struck heading~~"),
    ("strikethrough_in_blockquote",   "> ~~quoted~~"),
    ("strikethrough_with_escape",     "\\~~not strike~~"),
    ("strikethrough_with_code",       "~~before `tilde~~here` after~~"),
    ("strikethrough_across_words",    "foo~~bar~~baz"),
    ("strikethrough_lone_marker_left",
     "~~~strike~~"),

    # GFM-extension: tasklists. Disabled by default; the C side mirrors
    # `MarkdownIt('default', {'tasklists': True})`.
    ("tasklists_basic_unchecked", "- [ ] item",          {"tasklists": True}),
    ("tasklists_basic_checked",   "- [x] item",          {"tasklists": True}),
    ("tasklists_basic_checked_X", "- [X] item",          {"tasklists": True}),
    ("tasklists_mixed",
     "- [ ] one\n- [x] two\n- three",                    {"tasklists": True}),
    ("tasklists_ordered",         "1. [ ] first",        {"tasklists": True}),
    ("tasklists_no_whitespace_after",
     "- [x]nope",                                        {"tasklists": True}),
    ("tasklists_only_brackets",   "- []",                {"tasklists": True}),
    ("tasklists_inner_invalid",   "- [Y] item",          {"tasklists": True}),
    ("tasklists_disabled_default", "- [ ] item"),  # no opts -> not a checkbox
    ("tasklists_editable",
     "- [x] one",                                        {"tasklists": True,
                                                          "tasklists_editable": True}),
    ("tasklists_no_class_when_no_check",
     "- regular item",                                   {"tasklists": True}),
    ("tasklists_indented",
     "  - [ ] indented one\n  - [x] indented two",       {"tasklists": True}),
    ("tasklists_with_nested_list",
     "- [ ] outer\n  - inner",                           {"tasklists": True}),
    ("tasklists_loose",
     "- [ ] one\n\n- [x] two",                           {"tasklists": True}),
    ("tasklists_with_emphasis",
     "- [x] _italic_ item",                              {"tasklists": True}),

    # GFM-extension: alerts. Disabled by default; mirrors
    # `MarkdownIt('default', {'alerts': True})`.
    ("alerts_note",
     "> [!NOTE]\n> Useful information that users should know.",
     {"alerts": True}),
    ("alerts_tip",
     "> [!TIP]\n> Helpful advice for doing things better.",
     {"alerts": True}),
    ("alerts_important",
     "> [!IMPORTANT]\n> Key information users need to know.",
     {"alerts": True}),
    ("alerts_warning",
     "> [!WARNING]\n> Critical content demanding attention.",
     {"alerts": True}),
    ("alerts_caution",
     "> [!CAUTION]\n> Negative potential consequences of an action.",
     {"alerts": True}),
    ("alerts_lowercase_kind",
     "> [!note]\n> case-insensitive marker.",
     {"alerts": True}),
    ("alerts_no_content",
     "> [!NOTE]",
     {"alerts": True}),  # no body line -> stays a blockquote
    ("alerts_unknown_kind",
     "> [!FOO]\n> body",
     {"alerts": True}),
    ("alerts_disabled_default",
     "> [!NOTE]\n> body"),  # no opts -> renders as blockquote
    ("alerts_with_inline",
     "> [!TIP]\n> *italic* body",
     {"alerts": True}),
    ("alerts_multiline",
     "> [!WARNING]\n> first\n> second",
     {"alerts": True}),
    ("alerts_nested_blockquote",
     "> [!NOTE]\n> > nested",
     {"alerts": True}),
    ("alerts_not_first_line",
     "> body\n> [!NOTE] second",  # marker not on first content line
     {"alerts": True}),
    ("alerts_with_extra_brackets",
     "> [![NOTE]] body",
     {"alerts": True}),

    # Inline rule: autolink (`<scheme:rest>` / `<email>`)
    ("autolink_url",                  "<http://example.com>"),
    ("autolink_url_https",            "<https://example.com/path?q=1>"),
    ("autolink_url_with_ftp",         "<ftp://ftp.example.com/file.txt>"),
    ("autolink_url_short_scheme",     "<a:b>"),  # 2-char scheme, valid
    ("autolink_url_long_path",        "<http://example.com/a/b/c/d>"),
    ("autolink_url_one_char_scheme",  "<a:foo>"),  # 1-char scheme — valid (>=2 incl lead)
    ("autolink_url_too_short_scheme", "<a>"),  # bare host, no `:` — falls through
    ("autolink_url_with_query_frag",  "<http://example.com/p?q=1#f>"),
    ("autolink_url_disallow_space",   "<http://example.com/ foo>"),
    ("autolink_url_disallow_lt",      "<http://exa<mple.com>"),
    ("autolink_url_in_paragraph",     "see <http://example.com> for more"),
    ("autolink_email",                "<foo@example.com>"),
    ("autolink_email_subdomain",      "<a.b@sub.example.co.uk>"),
    ("autolink_email_dotless",        "<foo@bar>"),  # invalid — host needs at least one label
    ("autolink_email_with_plus",      "<a+b@example.com>"),
    ("autolink_email_in_paragraph",   "mail <foo@example.com> me"),
    ("autolink_invalid_no_close",     "<http://example.com"),
    ("autolink_invalid_empty",        "<>"),
    ("autolink_invalid_bare",         "<not-a-url>"),
    ("autolink_invalid_javascript",   "<javascript:alert(1)>"),
    ("autolink_with_emphasis",        "*see <http://x.example>*"),
    ("autolink_url_unicode_path",     "<http://example.com/π>"),
    ("autolink_url_with_html",        "<http://example.com>", {"html": True}),
    ("autolink_inside_html",          "before <a> <http://example.com> </a>", {"html": True}),

    # Inline rule: link
    ("link_inline_basic",             "[foo](http://x.example)"),
    ("link_inline_title",             "[foo](http://x.example \"title\")"),
    ("link_inline_no_dest",           "[foo]()"),
    ("link_inline_paren_dest",        "[foo](</a b>)"),
    ("link_inline_with_emph",         "[*a*](http://x.example)"),
    ("link_inline_with_code",         "[`a`](http://x.example)"),
    ("link_inline_unbalanced",        "[foo](http://x.example"),
    ("link_inline_javascript",        "[foo](javascript:alert(1))"),
    ("link_inline_data_image",
     "[foo](data:image/png;base64,iVBORw0KGgo)"),
    ("link_in_paragraph",             "see [foo](http://x.example) for more"),
    ("link_in_heading",               "# [foo](http://x.example)"),
    ("link_in_list",                  "- [foo](http://x.example)"),
    ("link_in_blockquote",            "> [foo](http://x.example)"),
    ("link_full_reference",
     "[foo][bar]\n\n[bar]: http://x.example \"title\""),
    ("link_full_reference_no_title",
     "[foo][bar]\n\n[bar]: http://x.example"),
    ("link_collapsed_reference",
     "[foo][]\n\n[foo]: http://x.example"),
    ("link_shortcut_reference",
     "[foo]\n\n[foo]: http://x.example"),
    ("link_reference_missing",        "[foo][bar]\n\nno reference here"),
    ("link_label_case_insensitive",
     "[FOO][bar]\n\n[BAR]: http://x.example"),
    ("link_label_whitespace_collapse",
     "[foo][  bar  baz  ]\n\n[bar baz]: http://x.example"),
    ("link_with_escapes",             "[a\\]b](http://x.example)"),
    ("link_with_inline_brackets",     "[a[b]c](http://x.example)"),
    ("link_nested_inline_disallowed",
     "[a [b](http://y.example) c](http://x.example)"),
    ("link_empty_label",              "[](http://x.example)"),

    # Inline rule: image
    ("image_inline_basic",            "![alt](http://x.example/i.png)"),
    ("image_inline_title",
     "![alt](http://x.example/i.png \"title\")"),
    ("image_inline_with_emph",        "![*a*](i.png)"),
    ("image_inline_no_alt",           "![](i.png)"),
    ("image_in_paragraph",            "see ![alt](i.png) here"),
    ("image_in_link",
     "[![alt](i.png)](http://x.example)"),
    ("image_full_reference",
     "![alt][a]\n\n[a]: i.png \"title\""),
    ("image_collapsed_reference",
     "![alt][]\n\n[alt]: i.png"),
    ("image_shortcut_reference",
     "![alt]\n\n[alt]: i.png"),
    ("image_reference_missing",       "![alt][nope]\n\nno"),
    ("image_alt_with_inline_md",
     "![*hi* `code`](i.png)"),
    ("image_invalid_no_paren",        "![alt"),
    ("image_invalid_no_dest",         "![alt]"),

    # Linkify: inline rule (`://` trigger) + core rule (post-process).
    # All gated on options.linkify=True. The C port ships a built-in
    # default linkifier that handles explicit-scheme URLs (http/https/ftp,
    # mailto:) and plain emails, plus a small TLD-aware fuzzy-host pass.
    # Cases that depend on the full ICANN TLD list, IDN punycode, or
    # custom user schemas are intentionally absent here -- they belong
    # to a follow-up slice that ports the rest of linkify-it-py.
    ("linkify_inline_http",            "http://example.com",      {"linkify": True}),
    ("linkify_inline_https",           "https://example.com",     {"linkify": True}),
    ("linkify_inline_ftp",             "ftp://example.com",       {"linkify": True}),
    ("linkify_inline_in_paragraph",
     "Visit http://example.com today", {"linkify": True}),
    ("linkify_inline_with_path",
     "https://example.com/foo/bar?q=1", {"linkify": True}),
    ("linkify_inline_trailing_period",
     "see https://example.com.",       {"linkify": True}),
    ("linkify_inline_paren_balanced",
     "(http://example.com/a(b)c)",     {"linkify": True}),
    ("linkify_inline_trailing_star",
     "http://example.com/foo*",        {"linkify": True}),
    ("linkify_invalid_scheme_only",
     "scheme://x today",               {"linkify": True}),
    ("linkify_inside_autolink",
     "<http://example.com>",           {"linkify": True}),
    ("linkify_skipped_in_link",
     "[http://example.com](http://other.example)", {"linkify": True}),
    ("linkify_email_explicit",
     "mailto:foo@example.com",         {"linkify": True}),
    ("linkify_email_fuzzy",
     "Email: foo@example.com",         {"linkify": True}),
    ("linkify_fuzzy_host",
     "Visit example.com today",        {"linkify": True}),
    ("linkify_fuzzy_host_with_path",
     "Go to example.com/foo today",    {"linkify": True}),
    ("linkify_disabled_default",       "http://example.com"),
    ("linkify_in_blockquote",
     "> Visit http://example.com",     {"linkify": True}),
    ("linkify_in_list",
     "- http://example.com",           {"linkify": True}),
    ("linkify_in_table",
     "| a | b |\n|---|---|\n| http://x.example | y |",
     {"linkify": True}),
    ("linkify_with_emphasis",
     "**http://example.com/foo**",     {"linkify": True}),
    ("linkify_two_in_one_text",
     "see http://a.example and http://b.example",
     {"linkify": True}),

    # Linkify: IDN Punycode round-trip. Hostnames pass through
    # `mdurl` + `_punycode.to_ascii` for the href and
    # `_punycode.to_unicode` for the visible text. Encode-side: the
    # Cyrillic label is emitted as `xn--<...>` in the href. Decode-
    # side: any incoming `xn--<...>` label is turned back into Unicode
    # in the visible text. Both directions exercise our RFC 3492
    # codec (`mdit_punycode_encode_cps` / `mdit_punycode_decode`).
    ("linkify_idn_autolink_cyrillic",
     "<http://президент.рф/>",
     {"linkify": True}),
    ("linkify_idn_fuzzy_cyrillic_in_text",
     "visit http://сайт.рф/ today",
     {"linkify": True}),
    ("linkify_idn_xn_decode_in_display",
     "<https://example.xn--p1ai/path>",
     {"linkify": True}),
    ("linkify_idn_xn_decode_fuzzy",
     "go to example.xn--p1ai/path now",
     {"linkify": True}),
    ("linkify_idn_mailto_cyrillic_host",
     "<mailto:user@президент.рф>",
     {"linkify": True}),
    ("linkify_idn_mixed_labels",
     "<http://нрф.рф/>",
     {"linkify": True}),

    # Linkify: full ICANN TLD opt-in. Without `__full_tlds__`, brand
    # TLDs (`.app`, `.museum`, `.dev`) and many ccTLDs aren't matched
    # in fuzzy mode; with the toggle, they are. We pin both sides to
    # catch regressions in either direction.
    ("linkify_app_default_off",
     "go to example.app today",
     {"linkify": True}),
    ("linkify_app_with_full_tlds",
     "go to example.app today",
     {"linkify": True, "__full_tlds__": True}),
    ("linkify_dev_default_off",
     "see foo.dev now",
     {"linkify": True}),
    ("linkify_dev_with_full_tlds",
     "see foo.dev now",
     {"linkify": True, "__full_tlds__": True}),
    ("linkify_museum_default_yes",
     "art.museum is real",
     {"linkify": True}),
    ("linkify_museum_with_full_tlds",
     "art.museum is real",
     {"linkify": True, "__full_tlds__": True}),
    ("linkify_brand_email_full_tlds",
     "ping me at user@example.app",
     {"linkify": True, "__full_tlds__": True}),
    ("linkify_brand_with_path_full_tlds",
     "open https://docs.dev/getting-started",
     {"linkify": True, "__full_tlds__": True}),

    # Typographer: scoped abbreviations + rare punctuation runs.
    # Both rules `replacements` and `smartquotes` are gated on
    # `options.typographer`. With the option off, all of these stay
    # straight ASCII.
    ("typo_scoped_c_lower",       "(c) is a copyright",   {"typographer": True}),
    ("typo_scoped_c_upper",       "(C) is a copyright",   {"typographer": True}),
    ("typo_scoped_r_lower",       "(r) registered",       {"typographer": True}),
    ("typo_scoped_r_upper",       "(R) registered",       {"typographer": True}),
    ("typo_scoped_tm_lower",      "(tm) trademark",       {"typographer": True}),
    ("typo_scoped_tm_upper",      "(TM) trademark",       {"typographer": True}),
    ("typo_scoped_off",           "(c) is a copyright"),
    ("typo_plus_minus",           "+- 5 degrees",         {"typographer": True}),
    ("typo_ellipsis_two",         "wait..",               {"typographer": True}),
    ("typo_ellipsis_three",       "wait...",              {"typographer": True}),
    ("typo_ellipsis_many",        "wait......",           {"typographer": True}),
    ("typo_question_ellipsis",    "really?....",          {"typographer": True}),
    ("typo_excl_ellipsis",        "wow!....",             {"typographer": True}),
    ("typo_question_run",         "really?????",          {"typographer": True}),
    ("typo_excl_run",             "wow!!!!!!",            {"typographer": True}),
    ("typo_comma_run",            "yes,,,sure",           {"typographer": True}),
    ("typo_em_dash",              "left---right",         {"typographer": True}),
    ("typo_em_dash_at_start",     "---right",             {"typographer": True}),
    ("typo_em_dash_at_end",       "left---",              {"typographer": True}),
    ("typo_em_dash_alone",        "---",                  {"typographer": True}),
    ("typo_en_dash_spaces",       "left -- right",        {"typographer": True}),
    ("typo_en_dash_indent",       "left--right",          {"typographer": True}),
    ("typo_en_dash_at_start",     "-- right",             {"typographer": True}),
    ("typo_en_dash_at_end",       "left --",              {"typographer": True}),
    ("typo_dashes_no_match",      "----",                 {"typographer": True}),
    ("typo_combo",                "(c) wait... yes,, --", {"typographer": True}),

    # Typographer: smartquotes. Apostrophe in middle of word becomes
    # a curly right-single-quote; balanced pairs become curly opens
    # and closes.
    ("sq_apostrophe",             "don't",                {"typographer": True}),
    ("sq_apostrophe_multi",       "it's a girl's day",    {"typographer": True}),
    ("sq_double_quote_basic",     "say \"hello\" please", {"typographer": True}),
    ("sq_single_quote_basic",     "say 'hello' please",   {"typographer": True}),
    ("sq_double_unmatched",       "say \"hello",          {"typographer": True}),
    ("sq_double_at_word_boundary","\"foo bar\"",          {"typographer": True}),
    ("sq_inch_special",           "1\"\" inch",           {"typographer": True}),
    ("sq_nested",                 "\"outer 'inner' rest\"", {"typographer": True}),
    ("sq_quote_off",              "don't say \"hi\""),
    ("sq_with_em",                "say *\"hello\"* please", {"typographer": True}),
    ("sq_at_paragraph_start",     "\"start of line\"",    {"typographer": True}),
    ("sq_quote_after_punct",      "(\"hi\")",             {"typographer": True}),
    ("sq_combo_with_repl",        "(c) said \"hi\"...",   {"typographer": True}),
]


def _safe_quote(b: bytes) -> str:
    """C-string-literal-safe rendering of arbitrary bytes."""
    out = []
    i = 0
    while i < len(b):
        c = b[i]
        if c == 0x09:
            out.append("\\t")
        elif c == 0x0A:
            out.append("\\n")
        elif c == 0x0D:
            out.append("\\r")
        elif c == 0x22:
            out.append("\\\"")
        elif c == 0x5C:
            out.append("\\\\")
        elif 0x20 <= c < 0x7F:
            out.append(chr(c))
        else:
            out.append(f"\\x{c:02x}")
            # Avoid \xHH absorbing the next hex digit.
            if i + 1 < len(b):
                nxt = b[i + 1]
                if (0x30 <= nxt <= 0x39 or
                        0x41 <= nxt <= 0x46 or
                        0x61 <= nxt <= 0x66):
                    out.append('" "')
        i += 1
    return '"' + "".join(out) + '"'


def _normalize(case: tuple) -> tuple[str, str, dict]:
    if len(case) == 2:
        title, src = case
        return title, src, {}
    title, src, opts = case
    return title, src, opts


def _emit_row(title: str, src: str, opts: dict) -> str:
    use_full_tlds = bool(opts.pop("__full_tlds__", False))
    md = MarkdownIt("default", opts)
    if opts.get("linkify") and use_full_tlds:
        # linkify-it-py default ctor doesn't enable the full ICANN TLD
        # list. Mirror the C side's `mdit_linkifier_default_use_full_tlds`
        # toggle by extending the linkifier's TLD set with the full pack.
        from linkify_it.tlds import TLDS as _ICANN_TLDS
        md.linkify.tlds(_ICANN_TLDS, keep_old=True)
    rendered = md.render(src)
    ib = src.encode("utf-8")
    hb = rendered.encode("utf-8")
    flags = []
    if opts.get("html"):        flags.append("MDIT_BLOCK_ORACLE_OPT_HTML")
    if opts.get("linkify"):     flags.append("MDIT_BLOCK_ORACLE_OPT_LINKIFY")
    if opts.get("typographer"): flags.append("MDIT_BLOCK_ORACLE_OPT_TYPOGRAPHER")
    if use_full_tlds:           flags.append("MDIT_BLOCK_ORACLE_OPT_FULL_TLDS")
    if opts.get("tasklists_editable"):
        flags.append("MDIT_BLOCK_ORACLE_OPT_TASKLISTS_EDITABLE")
    elif opts.get("tasklists"):
        flags.append("MDIT_BLOCK_ORACLE_OPT_TASKLISTS")
    if opts.get("alerts"):      flags.append("MDIT_BLOCK_ORACLE_OPT_ALERTS")
    flag_expr = " | ".join(flags) if flags else "0"
    return (
        "    { "
        f"\"{title}\", "
        f"{_safe_quote(ib)}, {len(ib)}, "
        f"{_safe_quote(hb)}, {len(hb)}, "
        f"{flag_expr} "
        "},"
    )


def main() -> None:
    lines = [
        "/* Auto-generated by scripts/gen_block_oracle.py — do not edit. */",
        "/* Cases: (title, input_md, expected_html, opts) cross-checked",
        " * against markdown_it.MarkdownIt('default', opts).render(). */",
        "#ifndef MDIT_TESTS_BLOCK_ORACLE_H",
        "#define MDIT_TESTS_BLOCK_ORACLE_H",
        "",
        "#define MDIT_BLOCK_ORACLE_OPT_HTML                0x01u",
        "#define MDIT_BLOCK_ORACLE_OPT_LINKIFY             0x02u",
        "#define MDIT_BLOCK_ORACLE_OPT_FULL_TLDS           0x04u",
        "#define MDIT_BLOCK_ORACLE_OPT_TYPOGRAPHER         0x08u",
        "#define MDIT_BLOCK_ORACLE_OPT_TASKLISTS           0x10u",
        "#define MDIT_BLOCK_ORACLE_OPT_TASKLISTS_EDITABLE  0x20u",
        "#define MDIT_BLOCK_ORACLE_OPT_ALERTS              0x40u",
        "",
        "typedef struct mdit_block_oracle_case {",
        "    const char *title;",
        "    const char *input;",
        "    size_t      input_len;",
        "    const char *html;",
        "    size_t      html_len;",
        "    unsigned    opts;",
        "} mdit_block_oracle_case;",
        "",
        "static const mdit_block_oracle_case MDIT_BLOCK_ORACLE[] = {",
    ]
    for case in CASES:
        title, src, opts = _normalize(case)
        lines.append(_emit_row(title, src, opts))
    lines.extend([
        "};",
        "",
        "static const size_t MDIT_BLOCK_ORACLE_LEN =",
        "    sizeof(MDIT_BLOCK_ORACLE) / sizeof(MDIT_BLOCK_ORACLE[0]);",
        "",
        "#endif /* MDIT_TESTS_BLOCK_ORACLE_H */",
        "",
    ])
    OUT.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print(f"wrote {OUT} with {len(CASES)} cases")


if __name__ == "__main__":
    main()
