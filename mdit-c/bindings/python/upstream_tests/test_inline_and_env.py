"""parseInline / renderInline + env-populating parse tests.

Mirrors a subset of ``tests/test_api/test_main.py`` that exercises the
inline-mode parse path and the way ``env`` is populated by the
reference rule.
"""

from __future__ import annotations

import pytest


def test_parseInline_token_shape():
    import mdit_c

    md = mdit_c.MarkdownIt()
    tokens = md.parseInline("abc\n\n> xyz")
    assert len(tokens) == 1
    inline = tokens[0]
    assert inline.type == "inline"
    assert inline.tag == ""
    assert inline.nesting == 0
    assert inline.map == [0, 1]
    assert inline.children is not None
    types = [child.type for child in inline.children]
    # Soft-breaks for the blank-line gap, then plain text spans.
    assert types[0] == "text"
    assert "softbreak" in types
    assert types[-1] == "text"
    assert inline.content == "abc\n\n> xyz"


def test_renderInline_skips_paragraph_wrap():
    import mdit_c

    md = mdit_c.MarkdownIt("zero")
    rendered = md.renderInline("abc\n\n*xyz*")
    # zero preset: emphasis disabled, so the literal `*xyz*` stays as
    # text. The return value must NOT be wrapped in <p>.
    assert rendered == "abc\n\n*xyz*"
    assert "<p>" not in rendered

    md = mdit_c.MarkdownIt()
    rendered = md.renderInline("**bold**")
    assert rendered == "<strong>bold</strong>"


def test_empty_str_parse_inline():
    import mdit_c

    md = mdit_c.MarkdownIt()
    tokens = md.parseInline("")
    assert len(tokens) == 1
    assert tokens[0].type == "inline"
    assert tokens[0].children == []
    assert tokens[0].content == ""


def test_empty_env_populated_by_parse():
    import mdit_c

    md = mdit_c.MarkdownIt()
    env: dict = {}
    md.render("[foo]: /url\n[foo]", env)
    assert "references" in env

    env = {}
    md.parse("[foo]: /url\n[foo]", env)
    assert "references" in env


def test_env_reference_shape_matches_upstream():
    pytest.importorskip("markdown_it")
    import markdown_it
    import mdit_c

    src = "[foo]: /url 'title'\n[bar]: /b\n\n[foo] and [bar]"
    e1, e2 = {}, {}
    out_c = mdit_c.MarkdownIt().render(src, e1)
    out_py = markdown_it.MarkdownIt().render(src, e2)
    assert out_c == out_py
    assert e1 == e2


def test_env_duplicate_refs_match_upstream():
    pytest.importorskip("markdown_it")
    import markdown_it
    import mdit_c

    src = "[foo]: /one\n[foo]: /two\n\n[foo]"
    e1, e2 = {}, {}
    mdit_c.MarkdownIt().render(src, e1)
    markdown_it.MarkdownIt().render(src, e2)
    assert e1 == e2


def test_non_mapping_env_raises():
    import mdit_c

    md = mdit_c.MarkdownIt()
    with pytest.raises(TypeError):
        md.parse("hi", "not a mapping")
    with pytest.raises(TypeError):
        md.render("hi", 42)
    with pytest.raises(TypeError):
        md.parseInline("hi", [])


def test_parseInline_parity_with_upstream():
    pytest.importorskip("markdown_it")
    import markdown_it
    import mdit_c

    inputs = [
        "abc\n\n> xyz",
        "**bold** _italic_",
        "[link](http://x)",
        "code: `inline`",
        "",
    ]
    md_c = mdit_c.MarkdownIt()
    md_py = markdown_it.MarkdownIt()
    for src in inputs:
        c_tokens = [t.as_dict(as_upstream=True) for t in md_c.parseInline(src)]
        py_tokens = [t.as_dict() for t in md_py.parseInline(src)]
        assert c_tokens == py_tokens, src
        assert md_c.renderInline(src) == md_py.renderInline(src), src
