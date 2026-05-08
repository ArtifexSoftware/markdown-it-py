"""Upstream-shaped tests for ``mdit_c.tree.SyntaxTreeNode``.

These mirror ``tests/test_tree.py`` from the markdown-it-py corpus,
exercising the syntax-tree wrapper against tokens produced by the
``mdit_c`` C engine.
"""

from __future__ import annotations


EXAMPLE_MARKDOWN = """
## Heading here

Some paragraph text and **emphasis here** and more text here.
"""


def test_tree_to_tokens_conversion():
    import mdit_c
    from mdit_c.tree import SyntaxTreeNode

    tokens = mdit_c.MarkdownIt().parse(EXAMPLE_MARKDOWN)
    tokens_after_roundtrip = SyntaxTreeNode(tokens).to_tokens()
    assert tokens == tokens_after_roundtrip


def test_property_passthrough():
    import mdit_c
    from mdit_c.tree import SyntaxTreeNode

    tokens = mdit_c.MarkdownIt().parse(EXAMPLE_MARKDOWN)
    heading_open = tokens[0]
    tree = SyntaxTreeNode(tokens)
    heading_node = tree.children[0]
    assert heading_open.tag == heading_node.tag
    assert tuple(heading_open.map or ()) == heading_node.map
    assert heading_open.level == heading_node.level
    assert heading_open.content == heading_node.content
    assert heading_open.markup == heading_node.markup
    assert heading_open.info == heading_node.info
    assert heading_open.meta == heading_node.meta
    assert heading_open.block == heading_node.block
    assert heading_open.hidden == heading_node.hidden


def test_type():
    import mdit_c
    from mdit_c.tree import SyntaxTreeNode

    tokens = mdit_c.MarkdownIt().parse(EXAMPLE_MARKDOWN)
    tree = SyntaxTreeNode(tokens)
    assert tree.type == "root"
    assert tree.children[0].type == "heading"
    assert tree[0].type == "heading"
    assert tree.children[0].children[0].type == "inline"


def test_sibling_traverse():
    import mdit_c
    from mdit_c.tree import SyntaxTreeNode

    tokens = mdit_c.MarkdownIt().parse(EXAMPLE_MARKDOWN)
    tree = SyntaxTreeNode(tokens)
    paragraph_inline_node = tree.children[1].children[0]
    text_node = paragraph_inline_node.children[0]
    assert text_node.type == "text"
    strong_node = text_node.next_sibling
    assert strong_node is not None
    assert strong_node.type == "strong"
    another_text_node = strong_node.next_sibling
    assert another_text_node is not None
    assert another_text_node.type == "text"
    assert another_text_node.next_sibling is None
    assert (
        another_text_node.previous_sibling is not None
        and another_text_node.previous_sibling.previous_sibling == text_node
    )
    assert text_node.previous_sibling is None


def test_walk():
    import mdit_c
    from mdit_c.tree import SyntaxTreeNode

    tokens = mdit_c.MarkdownIt().parse(EXAMPLE_MARKDOWN)
    tree = SyntaxTreeNode(tokens)
    expected_node_types = (
        "root",
        "heading",
        "inline",
        "text",
        "paragraph",
        "inline",
        "text",
        "strong",
        "text",
        "text",
    )
    for node, expected_type in zip(tree.walk(), expected_node_types):
        assert node.type == expected_type


def test_pretty_matches_upstream_xml():
    import mdit_c
    from mdit_c.tree import SyntaxTreeNode

    md = mdit_c.MarkdownIt("commonmark")
    tokens = md.parse(
        "\n# Header\n\nHere's some text and an image ![title](image.png)\n"
        "\n1. a **list**\n\n> a *quote*\n    "
    )
    expected = (
        "<root>\n"
        "  <heading>\n"
        "    <inline>\n"
        "      <text>\n"
        "        Header\n"
        "  <paragraph>\n"
        "    <inline>\n"
        "      <text>\n"
        "        Here's some text and an image \n"
        "      <image src='image.png' alt=''>\n"
        "        <text>\n"
        "          title\n"
        "  <ordered_list>\n"
        "    <list_item>\n"
        "      <paragraph>\n"
        "        <inline>\n"
        "          <text>\n"
        "            a \n"
        "          <strong>\n"
        "            <text>\n"
        "              list\n"
        "          <text>\n"
        "  <blockquote>\n"
        "    <paragraph>\n"
        "      <inline>\n"
        "        <text>\n"
        "          a \n"
        "        <em>\n"
        "          <text>\n"
        "            quote"
    )
    actual = SyntaxTreeNode(tokens).pretty(indent=2, show_text=True)
    assert actual == expected


def test_pretty_text_special_matches_upstream():
    import mdit_c
    from mdit_c.tree import SyntaxTreeNode

    md = mdit_c.MarkdownIt()
    md.disable("text_join")
    tree = SyntaxTreeNode(md.parse("foo &copy; bar \\("))
    expected = (
        "<root>\n"
        "  <paragraph>\n"
        "    <inline>\n"
        "      <text>\n"
        "        foo \n"
        "      <text_special>\n"
        "        \u00a9\n"
        "      <text>\n"
        "         bar \n"
        "      <text_special>\n"
        "        ("
    )
    assert tree.pretty(show_text=True) == expected


def test_root_node_has_no_attribute_pass_through():
    import pytest

    import mdit_c
    from mdit_c.tree import SyntaxTreeNode

    tokens = mdit_c.MarkdownIt().parse("# hi")
    tree = SyntaxTreeNode(tokens)
    assert tree.is_root
    with pytest.raises(AttributeError):
        _ = tree.tag
    with pytest.raises(AttributeError):
        _ = tree.content


def test_top_level_export():
    import mdit_c

    assert hasattr(mdit_c, "SyntaxTreeNode")
    tokens = mdit_c.MarkdownIt().parse("# hi")
    tree = mdit_c.SyntaxTreeNode(tokens)
    assert tree.children[0].type == "heading"
