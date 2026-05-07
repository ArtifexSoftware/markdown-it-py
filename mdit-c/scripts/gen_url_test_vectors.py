#!/usr/bin/env python3
"""
gen_url_test_vectors.py — emit ``mdit-c/tests/url_vectors.h``.

Cross-check fixtures for ``mdit_url_parse`` / ``mdit_url_format`` /
``mdit_url_encode`` / ``mdit_url_decode``.

Each test case is a (input, expected) pair derived from real
``mdurl`` output. The C test runs the corresponding port routine and
asserts byte equality. Drift (e.g. mdurl semantics changing on a
package upgrade) fails the build.

Coverage emphasis:
  - Trivial relative paths (``/foo``, ``./bar``).
  - HTTPS URLs with auth, port, IPv6, query, fragment.
  - The hostname-validation fallback that splits non-ASCII labels.
  - Hostless protocols (``javascript:alert(1)``).
  - Encode/decode pairs covering ASCII unreserved, ASCII reserved,
    Latin-1, multi-byte UTF-8 (€, 😀), and the keep-escaped %XX path.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUTPUT = REPO_ROOT / "mdit-c" / "tests" / "url_vectors.h"


PARSE_INPUTS = [
    "",
    "/",
    "//",
    "///",  # too many slashes — falls out of simple-path
    "/foo/bar",
    "./relative",
    "?query=1",
    "/path?q=1",
    "/path?q=1#frag",
    "http://example.com",
    "http://example.com/",
    "http://example.com/path?q=1#frag",
    "https://user:pass@example.com:8080/p?q#h",
    "ftp://files.example.com/dir/file.txt",
    "javascript:alert(1)",
    "javascript:void(0)",
    "//cdn.example.com/script.js",
    "  http://trim-me.test/  ",
    "mailto:foo@bar.test",
    "data:text/plain,hello",
    "http://[::1]:80/",
    "http://example.com:80",
    "http://example.com:",
    "http://x@y@example.com/path@inside",
    "http://exämple.com/path",   # unicode hostname
    "http://пример.рф/",          # full unicode
    "http://example.com/with space?q=1",  # with whitespace in path
    "irc://chat.example.com:6667/channel",
]

ENCODE_DEFAULT_CASES = [
    ("", ""),
    ("hello world", None),
    ("https://example.com/path?q=1", None),
    ("héllo", None),
    ("café/€", None),
    ("\u4e2d\u6587", None),                  # CJK
    ("\U0001F600 emoji", None),              # supplementary
    ("a%20b", None),                         # already-escaped
    ("%XYZ%2g", None),                       # malformed escapes
    ("100% pure", None),                     # raw % not followed by hex
    ("foo[]{}|\\^<>\"'", None),              # special punctuation
]

ENCODE_COMPONENT_CASES = [
    ("a/b?c=d&e=f", None),                   # default-allowed chars get encoded
    ("héllo&world", None),
]

DECODE_DEFAULT_CASES = [
    ("", ""),
    ("hello", None),
    ("hello%20world", None),
    ("h%C3%A9llo", None),                    # é
    ("caf%C3%A9", None),                     # café
    ("%E2%82%AC", None),                     # €
    ("%F0%9F%98%80", None),                  # 😀
    ("a%2fb", None),                         # decode lower-case
    ("a%2Fb", None),                         # excluded char (slash)
    ("a%3Bb", None),                         # excluded char (semicolon)
    ("invalid%XY", None),                    # malformed
    ("%C3%28", None),                        # invalid utf-8
    ("%E2%82", None),                        # truncated UTF-8
    ("a%20b%E2%82%ACc", None),
]

DECODE_COMPONENT_CASES = [
    ("a%2Fb", None),                          # slash not excluded → decoded
]


def _safe_quote(s: bytes) -> str:
    """Quote a bytes payload as a C string literal.

    We always close any \\xHH escape with an empty string concatenation
    (``"" ``) so the next byte can never be parsed as part of the escape.
    Spec §5.1.1.2 (translation phase 6) collapses adjacent string
    literals — this is safe and portable.
    """
    chunks: list[str] = []
    prev_was_hex = False
    for b in s:
        if b == ord('\\'):
            chunks.append('\\\\')
            prev_was_hex = False
        elif b == ord('"'):
            chunks.append('\\"')
            prev_was_hex = False
        elif 0x20 <= b < 0x7F:
            ch = chr(b)
            if prev_was_hex and ch in "0123456789abcdefABCDEF":
                # Close the previous \xHH so the new char isn't sucked
                # into the escape.
                chunks.append('" "')
            chunks.append(ch)
            prev_was_hex = False
        else:
            chunks.append(f"\\x{b:02x}")
            prev_was_hex = True
    return "".join(chunks)


def field_or_none(s: str | None) -> str:
    if s is None:
        return "{ false, NULL, 0 }"
    quoted = _safe_quote(s.encode("utf-8"))
    return f'{{ true, "{quoted}", {len(s.encode("utf-8"))} }}'


def render_parse_cases(mdurl):
    lines = []
    cases = []
    for url in PARSE_INPUTS:
        u = mdurl.parse(url, slashes_denote_host=False)
        formatted = mdurl.format(u)
        cases.append({
            "input": url,
            "protocol": u.protocol,
            "slashes": u.slashes,
            "auth": u.auth,
            "port": u.port,
            "hostname": u.hostname,
            "search": u.search,
            "hash": u.hash,
            "pathname": u.pathname,
            "format": formatted,
        })
    lines.append(f"typedef struct mdit_url_parse_case {{")
    lines.append("    const char *input;")
    lines.append("    size_t      input_len;")
    lines.append("    struct {")
    lines.append("        bool       present;")
    lines.append("        const char *data;")
    lines.append("        size_t      len;")
    lines.append("    } protocol, auth, port, hostname, search, hash, pathname;")
    lines.append("    bool        slashes;")
    lines.append("    const char *format;")
    lines.append("    size_t      format_len;")
    lines.append("} mdit_url_parse_case;")
    lines.append("")
    lines.append(f"static const mdit_url_parse_case k_url_parse_cases[{len(cases)}] = {{")
    for c in cases:
        ib = c["input"].encode("utf-8")
        fb = c["format"].encode("utf-8")
        lines.append("    {")
        lines.append(f'        "{_safe_quote(ib)}", {len(ib)},')
        for f in ("protocol", "auth", "port", "hostname", "search", "hash", "pathname"):
            lines.append(f"        {field_or_none(c[f])},")
        lines.append(f"        {'true' if c['slashes'] else 'false'},")
        lines.append(f'        "{_safe_quote(fb)}", {len(fb)}')
        lines.append("    },")
    lines.append("};")
    lines.append(f"static const size_t k_url_parse_cases_len = {len(cases)};")
    lines.append("")
    return "\n".join(lines)


_ENCODE_CASE_TYPE_EMITTED = [False]


def render_encode_cases(mdurl, name: str, exclude: str, cases):
    lines = []
    rows = []
    for inp, _ in cases:
        out = mdurl.encode(inp, exclude)
        rows.append((inp, out))
    if not _ENCODE_CASE_TYPE_EMITTED[0]:
        lines.append("typedef struct mdit_url_encode_case {")
        lines.append("    const char *input;  size_t input_len;")
        lines.append("    const char *output; size_t output_len;")
        lines.append("} mdit_url_encode_case;")
        lines.append("")
        _ENCODE_CASE_TYPE_EMITTED[0] = True
    lines.append(
        f"static const mdit_url_encode_case k_url_{name}_cases[{len(rows)}] = {{"
    )
    for inp, out in rows:
        ib = inp.encode("utf-8")
        ob = out.encode("utf-8")
        lines.append(
            f'    {{ "{_safe_quote(ib)}", {len(ib)}, "{_safe_quote(ob)}", {len(ob)} }},'
        )
    lines.append("};")
    lines.append(f"static const size_t k_url_{name}_cases_len = {len(rows)};")
    lines.append("")
    return "\n".join(lines)


def render_decode_cases(mdurl, name: str, exclude: str, cases):
    lines = []
    rows = []
    for inp, _ in cases:
        out = mdurl.decode(inp, exclude)
        rows.append((inp, out))
    lines.append(
        f"static const mdit_url_encode_case k_url_{name}_cases[{len(rows)}] = {{"
    )
    for inp, out in rows:
        ib = inp.encode("utf-8")
        ob = out.encode("utf-8")
        lines.append(
            f'    {{ "{_safe_quote(ib)}", {len(ib)}, "{_safe_quote(ob)}", {len(ob)} }},'
        )
    lines.append("};")
    lines.append(f"static const size_t k_url_{name}_cases_len = {len(rows)};")
    lines.append("")
    return "\n".join(lines)


def render(mdurl) -> str:
    lines: list[str] = []
    lines.append("/* AUTO-GENERATED by mdit-c/scripts/gen_url_test_vectors.py.")
    lines.append(" * DO NOT EDIT BY HAND.")
    lines.append(f" * mdurl version: {mdurl.__version__}")
    lines.append(" */")
    lines.append("#ifndef MDIT_TESTS_URL_VECTORS_H")
    lines.append("#define MDIT_TESTS_URL_VECTORS_H")
    lines.append("")
    lines.append("#include <stdbool.h>")
    lines.append("#include <stddef.h>")
    lines.append("")
    lines.append(render_parse_cases(mdurl))
    lines.append(render_encode_cases(mdurl, "encode_default",
                  mdurl.ENCODE_DEFAULT_CHARS, ENCODE_DEFAULT_CASES))
    lines.append(render_encode_cases(mdurl, "encode_component",
                  mdurl.ENCODE_COMPONENT_CHARS, ENCODE_COMPONENT_CASES))
    lines.append(render_decode_cases(mdurl, "decode_default",
                  mdurl.DECODE_DEFAULT_CHARS, DECODE_DEFAULT_CASES))
    lines.append(render_decode_cases(mdurl, "decode_component",
                  mdurl.DECODE_COMPONENT_CHARS, DECODE_COMPONENT_CASES))
    lines.append("#endif /* MDIT_TESTS_URL_VECTORS_H */")
    lines.append("")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output", type=Path, default=DEFAULT_OUTPUT,
        help=f"Where to write the C header (default: {DEFAULT_OUTPUT})",
    )
    args = parser.parse_args(argv)

    import mdurl
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(render(mdurl), encoding="utf-8", newline="\n")
    print(
        f"Wrote {args.output.relative_to(REPO_ROOT).as_posix()} "
        f"(mdurl {mdurl.__version__})."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
