"""Run the upstream CommonMark spec corpus against ``mdit_c.MarkdownIt``.

Mirrors ``tests/test_cmark_spec/test_spec.py`` from the upstream repo
but routes through the C engine via the ``_mdit_c`` extension.
"""
from __future__ import annotations

import json
from pathlib import Path

import pytest


def _load_spec_entries() -> list[dict]:
    repo_root = Path(__file__).resolve().parents[4]
    tests_input = repo_root / "tests" / "test_cmark_spec" / "commonmark.json"
    if not tests_input.is_file():
        return []
    return json.loads(tests_input.read_text(encoding="utf-8"))


SPEC_ENTRIES = _load_spec_entries()


@pytest.mark.parametrize(
    "entry",
    SPEC_ENTRIES,
    ids=[f"example-{e.get('example', '?')}" for e in SPEC_ENTRIES],
)
def test_spec(entry):
    import mdit_c

    md = mdit_c.MarkdownIt("commonmark")
    output = md.render(entry["markdown"])
    expected = entry["html"]

    # Two upstream-known cosmetic discrepancies; copy the workarounds
    # from upstream's test_spec.py verbatim so the C port has the same
    # exemptions.
    if entry["example"] == 596:
        output = output.replace("mailto", "MAILTO")
    if entry["example"] in [218, 239, 240]:
        output = output.replace(
            "<blockquote></blockquote>", "<blockquote>\n</blockquote>"
        )

    assert output == expected
