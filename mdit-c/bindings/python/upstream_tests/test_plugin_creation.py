"""Upstream-shaped tests for parser rule callbacks installed on the
``mdit_c`` engine via ``ruler.before``/``after``/``at``/``push``.

Mirrors ``tests/test_api/test_plugin_creation.py`` from the upstream
``markdown-it-py`` corpus. Covers:

* the Python rule is invoked with the right state-shaped wrapper and
  positional arguments,
* ``MarkdownIt.use`` is the canonical entry point,
* the user-supplied ``env`` argument from ``parse``/``render`` is
  forwarded as ``state.env``,
* ``state.tokens`` round-trips reads + mutations + reassignment,
* ``options={"alt": [...]}`` registers a rule under alt-chain tags,
* ``MarkdownIt.inline.add_terminator_char`` stops the inline ``text``
  rule on a new ASCII character.
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


# ---------------------------------------------------------------------------
# Env propagation into rule callbacks
# ---------------------------------------------------------------------------


def test_env_object_is_forwarded_to_core_rule():
    """The dict the caller hands to ``parse`` is the same object the
    rule callback sees on ``state.env``."""
    import mdit_c

    seen = {}
    user_env = {"caller": "alice"}

    def core_rule(state):
        seen["env"] = state.env

    def _plugin(_md):
        _md.core.ruler.after("normalize", "trace", core_rule)

    mdit_c.MarkdownIt().use(_plugin).parse("hello", user_env)
    assert seen["env"] is user_env


def test_env_is_forwarded_to_block_and_inline_rules():
    import mdit_c

    seen = []
    env = {"k": 1}

    def block_rule(state, startLine, endLine, silent):
        if not silent:
            seen.append(("block", state.env))
        return False

    def inline_rule(state, silent):
        if not silent:
            seen.append(("inline", state.env))
        return False

    def _plugin(_md):
        _md.block.ruler.before("paragraph", "be", block_rule)
        _md.inline.ruler.before("text", "ie", inline_rule)

    mdit_c.MarkdownIt().use(_plugin).parse("hi", env)
    assert ("block", env) in seen
    assert ("inline", env) in seen


def test_env_defaults_to_none_when_caller_omits_it():
    import mdit_c

    seen = {}

    def core_rule(state):
        seen["env"] = state.env

    def _plugin(_md):
        _md.core.ruler.after("normalize", "trace", core_rule)

    mdit_c.MarkdownIt().use(_plugin).parse("a")
    assert seen["env"] is None


# ---------------------------------------------------------------------------
# state.tokens read / write
# ---------------------------------------------------------------------------


def test_state_tokens_reads_existing_token_stream():
    """``state.tokens`` returns a list of Token instances mirroring the
    C-side stream after the prior rules have produced output."""
    import mdit_c
    from mdit_c import Token

    snapshot = {}

    def core_rule(state):
        toks = state.tokens
        snapshot["count"] = len(toks)
        snapshot["types"] = [t.type for t in toks]
        snapshot["all_tokens"] = all(isinstance(t, Token) for t in toks)

    def _plugin(_md):
        # Run after `inline` so the token stream is fully populated.
        _md.core.ruler.after("inline", "tap", core_rule)

    mdit_c.MarkdownIt().use(_plugin).parse("# hi")
    assert snapshot["count"] == 3
    assert snapshot["types"] == ["heading_open", "inline", "heading_close"]
    assert snapshot["all_tokens"] is True


def test_state_tokens_append_propagates_to_render():
    """Appending a Token in a Python core rule shows up in render output."""
    import mdit_c
    from mdit_c import Token

    def core_rule(state):
        # Append a fully-formed paragraph open/close pair so the
        # default renderer emits something we can grep for.
        state.tokens.append(Token("paragraph_open", "p", 1, block=True))
        inline = Token("inline", "", 0, block=True, content="injected")
        inline.children = [Token("text", "", 0, content="injected")]
        state.tokens.append(inline)
        state.tokens.append(Token("paragraph_close", "p", -1, block=True))

    def _plugin(_md):
        _md.core.ruler.after("inline", "inject", core_rule)

    html = mdit_c.MarkdownIt().use(_plugin).render("hello")
    assert "<p>hello</p>" in html
    assert "<p>injected</p>" in html


def test_state_tokens_assignment_replaces_stream():
    """``state.tokens = [...]`` swaps the entire stream wholesale."""
    import mdit_c
    from mdit_c import Token

    def core_rule(state):
        # Reduce a multi-token document to a single paragraph.
        para_open  = Token("paragraph_open", "p", 1, block=True)
        inline     = Token("inline", "", 0, block=True, content="sole")
        inline.children = [Token("text", "", 0, content="sole")]
        para_close = Token("paragraph_close", "p", -1, block=True)
        state.tokens = [para_open, inline, para_close]

    def _plugin(_md):
        _md.core.ruler.after("inline", "rewrite", core_rule)

    html = mdit_c.MarkdownIt().use(_plugin).render("# heading\n\npara")
    assert html == "<p>sole</p>\n"


def test_state_tokens_inline_children_round_trip():
    """``StateInline.tokens`` exposes the parent's children list and a
    Python-side append surfaces in the rendered output."""
    import mdit_c
    from mdit_c import Token

    fired = []

    def inline_rule(state, silent):
        if silent or state.pos >= state.posMax or state.src[state.pos] != "@":
            return False
        fired.append(state.pos)
        # Skip past '@' and emit a custom text token.
        state.pos += 1
        text = Token("text", "", 0, content="MENTION")
        # Append directly to the children list (the inline state's
        # `tokens` is the parent's `children`).
        state.tokens.append(text)
        return True

    def _plugin(_md):
        _md.inline.add_terminator_char("@")
        _md.inline.ruler.before("text", "mention", inline_rule)

    md = mdit_c.MarkdownIt().use(_plugin)
    html = md.render("hi @ there")
    assert fired == [3]  # byte offset of '@' inside "hi @ there"
    assert "MENTION" in html


# ---------------------------------------------------------------------------
# alt-chain options
# ---------------------------------------------------------------------------


def test_ruler_options_alt_is_accepted():
    """Registration with ``options={"alt": [...]}`` succeeds and the rule
    becomes part of the chain just like any other rule."""
    import mdit_c

    def block_rule(state, startLine, endLine, silent):
        return False

    md = mdit_c.MarkdownIt()
    md.block.ruler.before(
        "paragraph", "alt_rule", block_rule,
        {"alt": ["paragraph", "blockquote"]},
    )
    assert "alt_rule" in md.block.ruler.get_all_rules()
    assert "alt_rule" in md.block.ruler.get_active_rules()
    # And the parser still works end-to-end.
    assert "<p>hi</p>" in md.render("hi")


def test_ruler_options_unknown_key_raises():
    import mdit_c

    md = mdit_c.MarkdownIt()
    with pytest.raises(ValueError):
        md.core.ruler.after("normalize", "x", lambda _s: None,
                            {"not_alt": ["paragraph"]})


def test_ruler_options_alt_must_be_iterable_of_str():
    import mdit_c

    md = mdit_c.MarkdownIt()
    with pytest.raises(TypeError):
        md.core.ruler.after("normalize", "x", lambda _s: None, {"alt": 7})


# ---------------------------------------------------------------------------
# add_terminator_char
# ---------------------------------------------------------------------------


def test_add_terminator_char_single_letter():
    """Direct port of upstream ``test_add_terminator_char``: with the new
    terminator the inline rule fires at the registered character."""
    import mdit_c

    hit_positions = []

    def w_rule(state, silent):
        if state.src[state.pos] != "w":
            return False
        hit_positions.append(state.pos)
        state.pos += 1
        return True

    def _plugin(_md):
        _md.inline.add_terminator_char("w")
        _md.inline.ruler.before("text", "w_rule", w_rule)

    mdit_c.MarkdownIt().use(_plugin).render("awb")
    assert hit_positions == [1]


def test_add_terminator_char_rejects_non_ascii():
    import mdit_c

    md = mdit_c.MarkdownIt()
    with pytest.raises(ValueError):
        md.inline.add_terminator_char("\u00e9")


def test_add_terminator_char_rejects_non_single_char():
    import mdit_c

    md = mdit_c.MarkdownIt()
    with pytest.raises(ValueError):
        md.inline.add_terminator_char("ab")
    with pytest.raises(ValueError):
        md.inline.add_terminator_char("")

