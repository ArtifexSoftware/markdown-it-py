"""test_smoke.py — exercises the ``mdit_c`` extension end-to-end.

Run via CTest (driven by ``mdit-c/bindings/python/CMakeLists.txt``) or
directly:

    python test_smoke.py <stage_dir>

``stage_dir`` is the directory that contains the ``mdit_c`` package
(extension module + __init__.py). The CMake build copies both into
``<build>/python_stage`` after compiling the extension.

The smoke test does two things:

1. Imports ``mdit_c.MarkdownIt`` from the staged package, runs a few
   inputs through ``.render()`` and asserts the HTML against a
   hard-coded expected value (so the test doesn't depend on
   ``markdown_it`` being importable in the test interpreter).
2. If ``markdown_it`` *is* importable, additionally cross-checks
   byte-for-byte render parity and token ``as_dict(as_upstream=True)``
   parity against ``markdown_it.MarkdownIt('commonmark')`` for the same
   inputs.

Exit codes:
    0  all assertions held
    2  staged extension could not be imported
    3  hard-coded expectation mismatched
    4  parity vs. markdown_it mismatched
"""

from __future__ import annotations

import sys
from pathlib import Path


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print("usage: test_smoke.py <stage_dir>", file=sys.stderr)
        return 64

    stage = Path(argv[1]).resolve()
    if not stage.is_dir():
        print(f"smoke: stage directory not found: {stage}", file=sys.stderr)
        return 2
    sys.path.insert(0, str(stage))
    # Make the in-tree `markdown_it` package importable when the smoke
    # test is run from CTest's build-directory working dir. CI also
    # installs the package, but this keeps local developer runs useful.
    repo_root = Path(__file__).resolve().parents[3]
    sys.path.insert(1, str(repo_root))

    try:
        import mdit_c
    except Exception as exc:  # pragma: no cover - infrastructure failure
        print(f"smoke: failed to import mdit_c from {stage}: {exc!r}",
              file=sys.stderr)
        return 2

    print(f"smoke: mdit_c {mdit_c.__version__} loaded from {stage}")

    # Hard-coded expectations: the C engine in `commonmark` preset
    # mirrors `python -m markdown_it.cli.parse` byte-for-byte (the CLI
    # parity sweep already covers that). We re-use a small subset here
    # so this test passes even if `markdown_it` isn't installed.
    cases_commonmark = [
        ("# hi\n",                    "<h1>hi</h1>\n"),
        ("**bold** and *italic*",     "<p><strong>bold</strong> and <em>italic</em></p>\n"),
        ("[a](http://x.example)",     '<p><a href="http://x.example">a</a></p>\n'),
        ("- one\n- two\n",            "<ul>\n<li>one</li>\n<li>two</li>\n</ul>\n"),
        ("```py\nprint(1)\n```\n",
         '<pre><code class="language-py">print(1)\n</code></pre>\n'),
        ("> quoted\n",                "<blockquote>\n<p>quoted</p>\n</blockquote>\n"),
        ("---\n",                     "<hr />\n"),  # xhtmlOut=True for commonmark
    ]

    md = mdit_c.MarkdownIt("commonmark")
    for src, want in cases_commonmark:
        got = md.render(src)
        if got != want:
            print(f"smoke[commonmark] mismatch for {src!r}:\n"
                  f"  got:  {got!r}\n  want: {want!r}", file=sys.stderr)
            return 3

    # ``default`` preset enables the GFM `table` and `strikethrough`
    # rules but disables HTML by default (xhtmlOut=False, html=False).
    cases_default = [
        ("~~struck~~",                "<p><s>struck</s></p>\n"),
        ("---\n",                     "<hr>\n"),  # xhtmlOut=False -> bare <hr>
    ]
    md_default = mdit_c.MarkdownIt("default")
    for src, want in cases_default:
        got = md_default.render(src)
        if got != want:
            print(f"smoke[default] mismatch for {src!r}:\n"
                  f"  got:  {got!r}\n  want: {want!r}", file=sys.stderr)
            return 3

    # The persistent ``options`` dict is live: mutating it after
    # construction propagates to the C engine on the next render call.
    md_live = mdit_c.MarkdownIt("commonmark")
    if md_live.options["typographer"] is not False:
        print("smoke[options] typographer not False by default for commonmark",
              file=sys.stderr)
        return 3
    md_live.enable("replacements").enable("smartquotes")
    md_live.options["typographer"] = True
    sq_html = md_live.render('"hello"')
    if sq_html != "<p>\u201chello\u201d</p>\n":
        print(f"smoke[options] live typographer not picked up: {sq_html!r}",
              file=sys.stderr)
        return 3

    # gfm-like / gfm-like2 presets must select the right rule set +
    # option flags so an upstream-shaped MarkdownIt(config="gfm-like2")
    # works out of the box.
    md_gfm_like = mdit_c.MarkdownIt("gfm-like")
    if not md_gfm_like.options["linkify"]:
        print("smoke[gfm-like] linkify not enabled by preset",
              file=sys.stderr)
        return 3
    md_gfm2 = mdit_c.MarkdownIt("gfm-like2")
    for key in ("tasklists", "alerts", "strikethrough_single_tilde",
                "linkify"):
        if not md_gfm2.options[key]:
            print(f"smoke[gfm-like2] {key} not set by preset",
                  file=sys.stderr)
            return 3

    # Tasklists + alerts opt-ins.
    md_gfm = mdit_c.MarkdownIt("default", {"tasklists": True, "alerts": True})
    tlc_src = "- [x] done\n- [ ] todo\n"
    tlc_html = md_gfm.render(tlc_src)
    if "task-list-item" not in tlc_html or 'checked=""' not in tlc_html:
        print(f"smoke[gfm] tasklists rendered without expected markup:\n"
              f"  got: {tlc_html!r}", file=sys.stderr)
        return 3

    al_src = "> [!NOTE]\n> body\n"
    al_html = md_gfm.render(al_src)
    if "markdown-alert" not in al_html or "Note" not in al_html:
        print(f"smoke[gfm] alert rendered without expected markup:\n"
              f"  got: {al_html!r}", file=sys.stderr)
        return 3

    # The parse API returns Token objects with upstream-compatible
    # attributes and `as_dict(as_upstream=True)`.
    parsed = md.parse("# hi\n")
    if [tok.type for tok in parsed] != ["heading_open", "inline", "heading_close"]:
        print(f"smoke[parse] unexpected token types: {parsed!r}", file=sys.stderr)
        return 3
    if parsed[1].children is None or parsed[1].children[0].content != "hi":
        print("smoke[parse] missing inline text child", file=sys.stderr)
        return 3
    first_dict = parsed[0].as_dict(as_upstream=True)
    if first_dict["attrs"] is not None or first_dict["map"] != [0, 1]:
        print(f"smoke[parse] bad as_dict output: {first_dict!r}", file=sys.stderr)
        return 3
    parsed[0].attrSet("data-smoke", "ok")
    if parsed[0].attrGet("data-smoke") != "ok":
        print("smoke[parse] attrSet/attrGet mismatch", file=sys.stderr)
        return 3
    parsed[0].attrJoin("class", "one")
    parsed[0].attrJoin("class", "two")
    if parsed[0].attrGet("class") != "one two":
        print("smoke[parse] attrJoin mismatch", file=sys.stderr)
        return 3
    if parsed[0].attrIndex("data-smoke") < 0 or not parsed[0].attrItems():
        print("smoke[parse] attrIndex/attrItems mismatch", file=sys.stderr)
        return 3

    # Direct Token construction + round-trip helpers mirror
    # tests/test_api/test_token.py closely.
    tok = mdit_c.Token("name", "tag", 0, children=[mdit_c.Token("other", "tag2", 0)])
    tok.attrSet("a", "b")
    tok.attrJoin("a", "c")
    tok.attrPush(("x", "y"))
    if tok.attrGet("a") != "b c" or tok.attrIndex("x") != 1:
        print("smoke[token] attr helper mismatch", file=sys.stderr)
        return 3
    rebuilt = mdit_c.Token.from_dict(tok.as_dict())
    if tok != rebuilt:
        print("smoke[token] from_dict/as_dict round-trip mismatch", file=sys.stderr)
        return 3
    if tok.copy(content="changed").content != "changed":
        print("smoke[token] copy override mismatch", file=sys.stderr)
        return 3

    # Ruler facades expose upstream-shaped rule enumeration and
    # per-chain toggling. The top-level reset_rules context restores
    # all four chains (core/block/inline/inline2).
    md_zero = mdit_c.MarkdownIt("zero")
    all_rules = md_zero.get_all_rules()
    if "heading" not in all_rules["block"] or "fragments_join" not in all_rules["inline2"]:
        print(f"smoke[ruler] get_all_rules missing expected names: {all_rules!r}",
              file=sys.stderr)
        return 3
    if md_zero.block.ruler.enable("heading") != ["heading"]:
        print("smoke[ruler] block.ruler.enable did not return found rule",
              file=sys.stderr)
        return 3
    if "heading" not in md_zero.get_active_rules()["block"]:
        print("smoke[ruler] heading not enabled on block chain", file=sys.stderr)
        return 3
    active_with_heading = md_zero.get_active_rules()
    with md_zero.reset_rules():
        md_zero.disable("inline")
        if "inline" in md_zero.get_active_rules()["core"]:
            print("smoke[ruler] reset_rules body did not disable inline",
                  file=sys.stderr)
            return 3
    if md_zero.get_active_rules() != active_with_heading:
        # The context should restore to the state at entry (after the
        # explicit heading enable above), not to construction defaults.
        restored = md_zero.get_active_rules()
        print(f"smoke[ruler] reset_rules restore mismatch: {restored!r}",
              file=sys.stderr)
        return 3

    # Custom render rules are Python callbacks bound to md.renderer.
    # They receive Token copies, options, and env just like upstream,
    # and can delegate to self.renderToken after mutating the local
    # token copy.
    def paragraph_open(self, tokens, idx, options, env):
        assert env == {"smoke": True}
        tokens[idx].attrSet("data-render", "ok")
        return self.renderToken(tokens, idx, options, env)

    md_custom = mdit_c.MarkdownIt()
    md_custom.add_render_rule("paragraph_open", paragraph_open)
    rendered_custom = md_custom.render("hello", {"smoke": True})
    if rendered_custom != '<p data-render="ok">hello</p>\n':
        print(f"smoke[renderer] custom paragraph_open mismatch: "
              f"{rendered_custom!r}", file=sys.stderr)
        return 3

    # parseInline / renderInline. Inline-mode parsing produces a single
    # ``inline`` token whose ``children`` are the parsed inline tokens;
    # renderInline emits the inline content unwrapped by ``<p>``.
    md_inline = mdit_c.MarkdownIt("zero").enable(
        ["text", "newline", "emphasis", "balance_pairs", "fragments_join"])
    inline_tokens = md_inline.parseInline("abc\n\n*xyz*")
    if (len(inline_tokens) != 1 or inline_tokens[0].type != "inline"
            or inline_tokens[0].children is None):
        print(f"smoke[parseInline] unexpected token shape: {inline_tokens!r}",
              file=sys.stderr)
        return 3
    rendered_inline = md_inline.renderInline("abc\n\n*xyz*")
    if "<p>" in rendered_inline or "</p>" in rendered_inline:
        print(f"smoke[renderInline] should not produce <p>: "
              f"{rendered_inline!r}", file=sys.stderr)
        return 3

    # env-populating parse: link references are written back into
    # env["references"] in upstream's shape.
    md_refs = mdit_c.MarkdownIt()
    env: dict = {}
    md_refs.parse("[foo]: /url 'title'\n\n[foo]", env)
    if "references" not in env or "FOO" not in env["references"]:
        print(f"smoke[env] references missing: {env!r}", file=sys.stderr)
        return 3
    foo = env["references"]["FOO"]
    if foo["href"] != "/url" or foo["title"] != "title" or foo["map"] != [0, 1]:
        print(f"smoke[env] reference shape mismatch: {foo!r}", file=sys.stderr)
        return 3

    # Duplicate references go into env["duplicate_refs"].
    env_dup: dict = {}
    md_refs.parse("[foo]: /a\n[foo]: /b\n\n[foo]", env_dup)
    if (env_dup.get("references", {}).get("FOO", {}).get("href") != "/a"
            or len(env_dup.get("duplicate_refs", [])) != 1):
        print(f"smoke[env] duplicate refs mismatch: {env_dup!r}",
              file=sys.stderr)
        return 3

    # Validate non-mapping env raises TypeError, like upstream.
    try:
        md_refs.parse("hi", "not a mapping")
    except TypeError:
        pass
    else:
        print("smoke[env] non-mapping env should raise TypeError",
              file=sys.stderr)
        return 3

    # Optional: cross-check byte-parity against upstream Python.
    try:
        from markdown_it import MarkdownIt as PyMarkdownIt
    except Exception:  # pragma: no cover - environment-dependent
        print("smoke: markdown_it not importable; skipping parity sweep")
        print("smoke: PASS")
        return 0

    parity_inputs = [src for src, _ in cases_commonmark]
    parity_inputs.extend([
        "para 1\n\npara 2\n",
        "1. ordered\n2. items\n",
        "**a *b* c**",
        "`inline code`",
        "<http://example.com>",
        "Text with [a link](http://example.com) and **emph**.",
        "[![alt](img.png)](http://x)",
    ])

    py_md = PyMarkdownIt("commonmark")
    c_md = mdit_c.MarkdownIt("commonmark")
    for src in parity_inputs:
        want_html = py_md.render(src)
        got_html = c_md.render(src)
        if got_html != want_html:
            print(f"smoke[parity] mismatch for {src!r}:\n"
                  f"  got:  {got_html!r}\n  want: {want_html!r}", file=sys.stderr)
            return 4

        want_tokens = [t.as_dict(as_upstream=True) for t in py_md.parse(src)]
        got_tokens = [t.as_dict(as_upstream=True) for t in c_md.parse(src)]
        if got_tokens != want_tokens:
            print(f"smoke[token parity] mismatch for {src!r}:\n"
                  f"  got:  {got_tokens!r}\n  want: {want_tokens!r}",
                  file=sys.stderr)
            return 4

    print(f"smoke: PASS ({len(cases_commonmark)} commonmark + "
          f"{len(cases_default)} default + parity sweep "
          f"{len(parity_inputs)} cases)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
