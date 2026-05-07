"""End-to-end byte parity check: ``markdown-it-c`` vs ``python -m markdown_it.cli.parse``.

Runs both CLIs over a small bank of representative Markdown inputs and
asserts byte-for-byte equality on stdout. This is the fastest way to
catch regressions where the C port diverges from the Python reference
on something the unit-test corpus doesn't exercise.

Invoked from CTest as ``python test_cli_parity.py <md_cli_path>``. The
script imports ``markdown_it`` from the surrounding repo (no install
required) so CI can run it directly out of the source tree.
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


# Each tuple is (label, markdown_input). Pick inputs that exercise
# distinct rule chains (block + inline + edge-case escapes) so a single
# regression triggers a single labelled failure.
SAMPLES: list[tuple[str, str]] = [
    ("hello", "Hello, world!\n"),
    ("heading-mix", "# h1\n\n## h2\n\nparagraph\n"),
    ("bold-italic", "**bold** and *italic* and ***both***\n"),
    ("inline-code", "Use `printf(...)` to print, not `console.log`.\n"),
    ("hardbreak", "line one  \nline two\n"),
    ("escape", "use \\* not \\\\* and \\<foo> too\n"),
    ("blockquote", "> top\n> > nested\n> back to top\n"),
    ("bullet", "- one\n- two\n  - nested\n"),
    ("ordered", "1. a\n2. b\n3. c\n"),
    ("fenced", "```python\nprint('hi')\n```\n"),
    ("indented-code", "    int main(void) { return 0; }\n"),
    ("link-title", '[example](https://example.org "title")\n'),
    ("autolink", "Visit <https://example.org/path?q=1> please.\n"),
    ("autolink-email", "Mail <user@example.com> for help.\n"),
    ("image", "![alt](https://example.org/x.png)\n"),
    ("hr", "---\n"),
    ("entities", "&amp; &copy; &#x41; &#42; &bogus;\n"),
    ("inline-html", "see <span>raw</span> here\n"),
    ("html-block", "<div class=\"x\">\n  raw\n</div>\n"),
    ("reference",
        "see [link][slug]\n\n[slug]: https://example.org \"t\"\n"),
    ("crlf", "alpha\r\nbeta\r\n\r\ngamma\r\n"),
    ("nul-replace", "before\x00after\n"),
    ("setext-heading", "Title\n=====\n\nSubtitle\n--------\n"),
    ("nested-emph", "*** ___ ___ ***\n"),
    ("unicode", "Π is pi, π too. День добрый.\n"),
    ("table-ignored",
        "table preset is off in commonmark, so this is a paragraph:\n\n"
        "| a | b |\n|---|---|\n| 1 | 2 |\n"),
]


def _run(cmd: list[str], src: str, **kw: object) -> bytes:
    """Run ``cmd`` with ``src`` on stdin and return stdout as raw bytes.

    Bytes mode (not text mode) is intentional: on Windows, text mode
    re-encodes ``\\r\\n`` and force-converts to the console codepage,
    which would corrupt both the CR/LF normalization checks and the
    UTF-8 fixtures.
    """
    proc = subprocess.run(
        cmd,
        input=src.encode("utf-8"),
        capture_output=True,
        check=True,
        **kw,  # type: ignore[arg-type]
    )
    return proc.stdout


def render_python(text: str) -> bytes:
    return _run(
        [sys.executable, "-m", "markdown_it.cli.parse", "--stdin"],
        text,
        cwd=str(REPO_ROOT),
    )


def render_c(binary: str, text: str) -> bytes:
    return _run([binary, "--stdin"], text)


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print("usage: test_cli_parity.py <markdown-it-c binary>",
              file=sys.stderr)
        return 64
    binary = argv[1]
    if not os.path.exists(binary):
        print(f"binary not found: {binary}", file=sys.stderr)
        return 64

    # Skip gracefully if the Python reference isn't importable. We do this
    # via a noop subprocess rather than ``import markdown_it`` here so the
    # test interpreter and the runtime interpreter are the same.
    probe = subprocess.run(
        [sys.executable, "-c", "import markdown_it"],
        cwd=str(REPO_ROOT),
        capture_output=True,
    )
    if probe.returncode != 0:
        print("SKIP: markdown_it not importable; install it to run "
              "the parity sweep.", flush=True)
        return 0

    failures: list[str] = []
    for label, src in SAMPLES:
        py = render_python(src)
        c = render_c(binary, src)
        if py != c:
            failures.append(label)
            print(f"--- MISMATCH [{label}] ---", file=sys.stderr)
            print("input:", repr(src), file=sys.stderr)
            print("python:", repr(py), file=sys.stderr)
            print("c:    ", repr(c), file=sys.stderr)

    if failures:
        print(f"\n{len(failures)} mismatch(es): {', '.join(failures)}",
              file=sys.stderr)
        return 1
    print(f"PASS: {len(SAMPLES)} cli parity cases", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
