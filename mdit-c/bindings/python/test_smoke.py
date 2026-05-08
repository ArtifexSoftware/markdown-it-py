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
   byte-for-byte parity against ``markdown_it.MarkdownIt('commonmark')``
   for the same inputs.

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
        want = py_md.render(src)
        got = c_md.render(src)
        if got != want:
            print(f"smoke[parity] mismatch for {src!r}:\n"
                  f"  got:  {got!r}\n  want: {want!r}", file=sys.stderr)
            return 4

    print(f"smoke: PASS ({len(cases_commonmark)} commonmark + "
          f"{len(cases_default)} default + parity sweep "
          f"{len(parity_inputs)} cases)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
