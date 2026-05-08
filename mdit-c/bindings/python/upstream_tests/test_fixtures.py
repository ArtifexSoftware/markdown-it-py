"""Mirrors ``tests/test_port/test_fixtures.py`` against the C engine.

Each fixture file under ``tests/test_port/fixtures/*.md`` is parsed
with the upstream ``read_fixture_file`` utility (which doesn't depend
on the parser at all), and then run through ``mdit_c.MarkdownIt``
configured the same way as the upstream test does. A byte-for-byte
match against the fixture's expected HTML is required.

A handful of upstream test bodies have a second half that exercises
runtime hooks the C engine doesn't expose yet (``md.linkify = None``
forcing ``ModuleNotFoundError`` is the canonical case). Those checks
are dropped in this port; the primary render parity assertion is
still performed.
"""
from __future__ import annotations

from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[4]
FIXTURE_PATH = REPO_ROOT / "tests" / "test_port" / "fixtures"


def _load(name):
    """Return ``read_fixture_file`` rows for the named fixture file.

    Resolved lazily so ``markdown_it`` isn't required at import time —
    when it's missing, ``conftest.py`` skips the whole module first.
    """
    from markdown_it.utils import read_fixture_file

    return read_fixture_file(FIXTURE_PATH / name)


@pytest.mark.parametrize("line,title,input,expected", _load("linkify.md"))
def test_linkify(line, title, input, expected):
    import mdit_c

    md = mdit_c.MarkdownIt().enable("linkify")
    md.options["linkify"] = True
    text = md.render(input)
    assert text.rstrip() == expected.rstrip()


@pytest.mark.parametrize("line,title,input,expected", _load("smartquotes.md"))
def test_smartquotes(line, title, input, expected):
    import mdit_c

    md = mdit_c.MarkdownIt().enable("replacements").enable("smartquotes")
    md.options["typographer"] = True
    text = md.render(input)
    assert text.rstrip() == expected.rstrip()


@pytest.mark.parametrize("line,title,input,expected", _load("typographer.md"))
def test_typographer(line, title, input, expected):
    import mdit_c

    md = mdit_c.MarkdownIt().enable("replacements")
    md.options["typographer"] = True
    text = md.render(input)
    assert text.rstrip() == expected.rstrip()


@pytest.mark.parametrize("line,title,input,expected", _load("tables.md"))
def test_table(line, title, input, expected):
    import mdit_c

    md = mdit_c.MarkdownIt().enable("table")
    text = md.render(input)
    assert text.rstrip() == expected.rstrip()


@pytest.mark.parametrize("line,title,input,expected",
                         _load("commonmark_extras.md"))
def test_commonmark_extras(line, title, input, expected):
    import mdit_c

    md = mdit_c.MarkdownIt("commonmark")
    md.options["langPrefix"] = ""
    text = md.render(input)
    assert text.rstrip() == expected.rstrip()


@pytest.mark.parametrize("line,title,input,expected", _load("normalize.md"))
def test_normalize_url(line, title, input, expected):
    import mdit_c

    md = mdit_c.MarkdownIt("commonmark")
    text = md.render(input)
    assert text.rstrip() == expected.rstrip()


@pytest.mark.parametrize("line,title,input,expected", _load("fatal.md"))
def test_fatal(line, title, input, expected):
    import mdit_c

    md = mdit_c.MarkdownIt("commonmark").enable("replacements")
    md.options["typographer"] = True
    text = md.render(input)
    assert text.rstrip() == expected.rstrip()


@pytest.mark.parametrize("line,title,input,expected",
                         _load("strikethrough.md"))
def test_strikethrough(line, title, input, expected):
    import mdit_c

    md = mdit_c.MarkdownIt().enable("strikethrough")
    text = md.render(input)
    assert text.rstrip() == expected.rstrip()


@pytest.mark.parametrize(
    "line,title,input,expected",
    _load("strikethrough_single_tilde.md"),
)
def test_strikethrough_single_tilde(line, title, input, expected):
    import mdit_c

    md = mdit_c.MarkdownIt("gfm-like2")
    text = md.render(input)
    assert text.rstrip() == expected.rstrip()


@pytest.mark.parametrize("line,title,input,expected",
                         _load("disable_code_block.md"))
def test_disable_code_block(line, title, input, expected):
    import mdit_c

    md = mdit_c.MarkdownIt().enable("table").disable("code")
    text = md.render(input)
    assert text.rstrip() == expected.rstrip()


@pytest.mark.parametrize("line,title,input,expected", _load("tasklists.md"))
def test_tasklists(line, title, input, expected):
    import mdit_c

    md = mdit_c.MarkdownIt("gfm-like2")
    text = md.render(input)
    assert text.rstrip() == expected.rstrip()


@pytest.mark.parametrize("line,title,input,expected", _load("alerts.md"))
def test_alerts(line, title, input, expected):
    import mdit_c

    md = mdit_c.MarkdownIt("gfm-like2")
    text = md.render(input)
    assert text.rstrip() == expected.rstrip()


@pytest.mark.parametrize("line,title,input,expected", _load("issue-fixes.md"))
def test_issue_fixes(line, title, input, expected):
    import mdit_c

    md = mdit_c.MarkdownIt()
    text = md.render(input)
    assert text.rstrip() == expected.rstrip()
