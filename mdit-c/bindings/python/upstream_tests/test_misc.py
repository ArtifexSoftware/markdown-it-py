"""Mirrors a subset of ``tests/test_port/test_misc.py`` against the
C extension. Tests that require Python-level rendering hooks (e.g.
``test_highlight_arguments``) are intentionally skipped — those land
when the renderer/ruler facades are exposed in a later slice.
"""
from __future__ import annotations


def test_ordered_list_info():
    import mdit_c

    def type_filter(tokens, type_):
        return [t for t in tokens if t.type == type_]

    md = mdit_c.MarkdownIt()

    tokens = md.parse("1. Foo\n2. Bar\n20. Fuzz")
    assert len(type_filter(tokens, "ordered_list_open")) == 1
    items = type_filter(tokens, "list_item_open")
    assert len(items) == 3
    assert items[0].info == "1"
    assert items[0].markup == "."
    assert items[1].info == "2"
    assert items[1].markup == "."
    assert items[2].info == "20"
    assert items[2].markup == "."

    tokens = md.parse(" 1. Foo\n2. Bar\n  20. Fuzz\n 199. Flp")
    assert len(type_filter(tokens, "ordered_list_open")) == 1
    items = type_filter(tokens, "list_item_open")
    assert len(items) == 4
    assert items[0].info == "1"
    assert items[0].markup == "."
    assert items[1].info == "2"
    assert items[1].markup == "."
    assert items[2].info == "20"
    assert items[2].markup == "."
    assert items[3].info == "199"
    assert items[3].markup == "."
