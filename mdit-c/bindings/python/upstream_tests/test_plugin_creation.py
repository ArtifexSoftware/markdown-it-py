"""Upstream-shaped tests for parser rule callbacks installed on the
``mdit_c`` engine via ``ruler.before``/``after``/``at``/``push``.

Mirrors ``tests/test_api/test_plugin_creation.py`` from the upstream
``markdown-it-py`` corpus, scoped to the surface implemented in slice
7: the Python rule is invoked with the right state-shaped wrapper and
positional arguments, and ``MarkdownIt.use`` is the canonical entry
point. Mutation of the token stream from a Python rule is deferred to
a follow-up slice.
"""

from __future__ import annotations

import pytest


def inline_rule(state, silent):
    print("plugin called", state.pos, silent)
    return False


def test_inline_after(capsys):
    import mdit_c

    def _plugin(_md):
        _md.inline.ruler.after("text", "new_rule", inline_rule)

    mdit_c.MarkdownIt().use(_plugin).parse("[")
    assert "plugin called" in capsys.readouterr().out


def test_inline_before(capsys):
    import mdit_c

    def _plugin(_md):
        _md.inline.ruler.before("text", "new_rule", inline_rule)

    mdit_c.MarkdownIt().use(_plugin).parse("a")
    assert "plugin called" in capsys.readouterr().out


def test_inline_at(capsys):
    import mdit_c

    def _plugin(_md):
        _md.inline.ruler.at("text", inline_rule)

    mdit_c.MarkdownIt().use(_plugin).parse("a")
    assert "plugin called" in capsys.readouterr().out


def block_rule(state, startLine, endLine, silent):
    print("plugin called", startLine, endLine, silent)
    return False


def test_block_after(capsys):
    import mdit_c

    def _plugin(_md):
        _md.block.ruler.after("hr", "new_rule", block_rule)

    mdit_c.MarkdownIt().use(_plugin).parse("a")
    assert "plugin called" in capsys.readouterr().out


def test_block_before(capsys):
    import mdit_c

    def _plugin(_md):
        _md.block.ruler.before("hr", "new_rule", block_rule)

    mdit_c.MarkdownIt().use(_plugin).parse("a")
    assert "plugin called" in capsys.readouterr().out


def test_block_at(capsys):
    import mdit_c

    def _plugin(_md):
        _md.block.ruler.at("hr", block_rule)

    mdit_c.MarkdownIt().use(_plugin).parse("a")
    assert "plugin called" in capsys.readouterr().out


def core_rule(state):
    print("plugin called", state.inlineMode)


def test_core_after(capsys):
    import mdit_c

    def _plugin(_md):
        _md.core.ruler.after("normalize", "new_rule", core_rule)

    mdit_c.MarkdownIt().use(_plugin).parse("a")
    assert "plugin called" in capsys.readouterr().out


def test_core_before(capsys):
    import mdit_c

    def _plugin(_md):
        _md.core.ruler.before("normalize", "new_rule", core_rule)

    mdit_c.MarkdownIt().use(_plugin).parse("a")
    assert "plugin called" in capsys.readouterr().out


def test_core_at(capsys):
    import mdit_c

    def _plugin(_md):
        _md.core.ruler.at("normalize", core_rule)

    mdit_c.MarkdownIt().use(_plugin).parse("a")
    assert "plugin called" in capsys.readouterr().out


def test_inline2_at(capsys):
    """``ruler2`` (post-process) chain accepts core-shaped Python rules."""
    import mdit_c

    seen = []

    def post(state):
        seen.append(state.src)

    def _plugin(_md):
        _md.inline.ruler2.at("balance_pairs", post)

    mdit_c.MarkdownIt().use(_plugin).parse("foo")
    assert seen == ["foo"]


def test_state_inline_pos_is_writable():
    """The inline state shim exposes ``pos`` as a writable attribute,
    enabling rules that consume input by advancing the cursor."""
    import mdit_c

    consumed = []

    def w_rule(state, silent):
        if silent:
            return False
        if state.pos >= state.posMax:
            return False
        ch = state.src[state.pos]
        if ch != "w":
            return False
        consumed.append(state.pos)
        state.pos += 1
        return True

    def _plugin(_md):
        _md.inline.ruler.before("text", "w_rule", w_rule)

    md = mdit_c.MarkdownIt().use(_plugin)
    md.render("wa")
    assert consumed == [0]


def test_core_state_attributes_match_upstream_shape():
    """Verify the read-only attributes the bridge exposes on StateCore."""
    import mdit_c

    captured = {}

    def core_rule(state):
        captured["src"] = state.src
        captured["inlineMode"] = state.inlineMode
        captured["md_is_self"] = state.md is md
        captured["env_type"] = type(state.env).__name__

    def _plugin(_md):
        _md.core.ruler.after("normalize", "capture", core_rule)

    md = mdit_c.MarkdownIt().use(_plugin)
    md.parse("# hi", {})
    assert captured["src"] == "# hi"
    assert captured["inlineMode"] is False
    assert captured["md_is_self"] is True


def test_state_invalidated_after_rule_returns():
    """Once the Python rule returns, accessing the captured state must
    raise — the underlying C state goes out of scope."""
    import mdit_c

    leaked = {}

    def core_rule(state):
        leaked["state"] = state

    def _plugin(_md):
        _md.core.ruler.after("normalize", "leak", core_rule)

    mdit_c.MarkdownIt().use(_plugin).parse("a")
    assert "state" in leaked
    with pytest.raises(RuntimeError):
        _ = leaked["state"].src


def test_block_state_basics():
    import mdit_c

    seen = []

    def block_rule(state, startLine, endLine, silent):
        seen.append((state.line, state.lineMax, state.blkIndent,
                     state.parentType, silent))
        return False

    def _plugin(_md):
        _md.block.ruler.before("paragraph", "trace", block_rule)

    mdit_c.MarkdownIt().use(_plugin).parse("hello")
    assert seen
    line, lineMax, blkIndent, parentType, silent = seen[0]
    assert line == 0
    assert lineMax == 1
    assert blkIndent == 0
    assert parentType == "root"
    assert silent is False


def test_invalid_chain_raises():
    import mdit_c

    md = mdit_c.MarkdownIt()
    with pytest.raises(KeyError):
        md._ruler_install("bogus", "push", "x", lambda *_: None)


def test_invalid_position_raises():
    import mdit_c

    md = mdit_c.MarkdownIt()
    with pytest.raises(ValueError):
        md._ruler_install("core", "sideways", "x", lambda *_: None)


def test_unknown_anchor_raises():
    import mdit_c

    md = mdit_c.MarkdownIt()
    with pytest.raises(KeyError):
        md.core.ruler.before("does_not_exist", "x", lambda *_: None)


def test_rule_can_be_disabled_then_reenabled(capsys):
    import mdit_c

    def core_rule(state):
        print("disabled-rule")

    def _plugin(_md):
        _md.core.ruler.after("normalize", "trace", core_rule)

    md = mdit_c.MarkdownIt().use(_plugin)
    md.disable("trace")
    md.parse("a")
    assert "disabled-rule" not in capsys.readouterr().out

    md.enable("trace")
    md.parse("a")
    assert "disabled-rule" in capsys.readouterr().out


