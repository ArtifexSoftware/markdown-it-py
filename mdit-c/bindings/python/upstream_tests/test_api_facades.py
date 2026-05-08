"""Focused upstream API tests for renderer/ruler facades.

This is not a wholesale copy of ``tests/test_api/test_main.py`` yet:
parser-state callbacks, ``parseInline`` / ``renderInline`` and
env-populating ``parse`` are follow-up slices. The cases here cover the
facade surface shipped by this slice.
"""

from __future__ import annotations


def test_get_rules():
    import mdit_c

    md = mdit_c.MarkdownIt("zero")
    assert md.get_all_rules() == {
        "core": [
            "normalize",
            "block",
            "inline",
            "linkify",
            "replacements",
            "smartquotes",
            "text_join",
        ],
        "block": [
            "table",
            "code",
            "fence",
            "blockquote",
            "hr",
            "list",
            "reference",
            "html_block",
            "heading",
            "lheading",
            "paragraph",
        ],
        "inline": [
            "text",
            "linkify",
            "newline",
            "escape",
            "backticks",
            "strikethrough",
            "emphasis",
            "link",
            "image",
            "autolink",
            "html_inline",
            "entity",
        ],
        "inline2": ["balance_pairs", "strikethrough", "emphasis", "fragments_join"],
    }


def test_load_presets():
    import mdit_c

    md = mdit_c.MarkdownIt("zero")
    assert md.get_active_rules() == {
        "block": ["paragraph"],
        "core": ["normalize", "block", "inline", "text_join"],
        "inline": ["text"],
        "inline2": ["balance_pairs", "fragments_join"],
    }
    md = mdit_c.MarkdownIt("commonmark")
    assert md.get_active_rules() == {
        "core": ["normalize", "block", "inline", "text_join"],
        "block": [
            "code",
            "fence",
            "blockquote",
            "hr",
            "list",
            "reference",
            "html_block",
            "heading",
            "lheading",
            "paragraph",
        ],
        "inline": [
            "text",
            "newline",
            "escape",
            "backticks",
            "emphasis",
            "link",
            "image",
            "autolink",
            "html_inline",
            "entity",
        ],
        "inline2": ["balance_pairs", "emphasis", "fragments_join"],
    }


def test_enable_disable_and_reset_rules():
    import mdit_c

    md = mdit_c.MarkdownIt("zero").enable("heading")
    assert md.get_active_rules()["block"] == ["heading", "paragraph"]
    md.enable(["backticks", "autolink"])
    assert md.get_active_rules()["inline"] == ["text", "backticks", "autolink"]

    with md.reset_rules():
        md.disable("inline")
        assert "inline" not in md.get_active_rules()["core"]

    assert md.get_active_rules()["core"] == ["normalize", "block", "inline", "text_join"]
    assert md.get_active_rules()["block"] == ["heading", "paragraph"]
    assert md.get_active_rules()["inline"] == ["text", "backticks", "autolink"]


def test_add_render_rule_delegates_to_render_token():
    import mdit_c

    def paragraph_open(self, tokens, idx, options, env):
        assert options["xhtmlOut"] is True
        assert env == {"source": "test"}
        tokens[idx].attrSet("data-custom", "yes")
        return self.renderToken(tokens, idx, options, env)

    md = mdit_c.MarkdownIt("commonmark")
    md.add_render_rule("paragraph_open", paragraph_open)
    assert md.render("hello", {"source": "test"}) == '<p data-custom="yes">hello</p>\n'


def test_use_can_install_render_plugin():
    import mdit_c

    def plugin(md):
        def text(self, tokens, idx, options, env):
            del options, env
            return tokens[idx].content.upper()

        md.add_render_rule("text", text)

    assert mdit_c.MarkdownIt().use(plugin).render("hello") == "<p>HELLO</p>\n"
