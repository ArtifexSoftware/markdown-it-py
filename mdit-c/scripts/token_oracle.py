#!/usr/bin/env python3
"""
token_oracle.py — generate the golden token/HTML corpus for the C port.

This Phase-0 tool walks the existing markdown-it-py test corpus and, for
every test case, captures:

* the input markdown,
* ``MarkdownIt(...).parse(input)`` flattened via ``Token.as_dict``,
* ``MarkdownIt(...).render(input)``.

The output lands in ``mdit-c/tests/oracle/<source>.jsonl`` (one JSON
record per line) plus a ``manifest.json`` index. The C port reads the
same JSONL files and asserts byte-for-byte equality during testing —
that is the contract the port must hold to.

Each oracle row looks like::

    {
      "source":   "commonmark_spec",
      "id":       12,
      "title":    "Tabs example 12",
      "preset":   "commonmark",
      "options":  {"langPrefix": "language-"},
      "enabled":  [],
      "disabled": [],
      "input":    "...",
      "tokens":   [ /* Token.as_dict() */ ],
      "html":     "...",
      "expected_html": "..."   # what the upstream test asserted, when known
    }

The ``expected_html`` field is included so the oracle is self-checking:
if Python's ``html`` differs from the upstream ``expected_html`` we
record both — the C port's job is to match Python, not the upstream
JS, when those disagree (rare but possible across versions).
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import sys
from collections.abc import Callable, Iterable, Iterator
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
TESTS_ROOT = REPO_ROOT / "tests"
DEFAULT_OUTPUT_DIR = REPO_ROOT / "mdit-c" / "tests" / "oracle"

# We import lazily so --help works without installing the package.
_MARKDOWN_IT = None


def _markdown_it_module() -> Any:
    global _MARKDOWN_IT
    if _MARKDOWN_IT is None:
        sys.path.insert(0, str(REPO_ROOT))
        import markdown_it  # noqa: PLC0415

        _MARKDOWN_IT = markdown_it
    return _MARKDOWN_IT


# ---------------------------------------------------------------------------
# MarkdownIt configurator descriptors — mirror tests/test_port/test_fixtures.py
# ---------------------------------------------------------------------------
@dataclasses.dataclass(frozen=True)
class Config:
    name: str
    preset: str = "default"
    enabled: tuple[str, ...] = ()
    disabled: tuple[str, ...] = ()
    options: tuple[tuple[str, Any], ...] = ()

    def build(self) -> Any:
        markdown_it = _markdown_it_module()
        md = markdown_it.MarkdownIt(self.preset)
        for rule in self.enabled:
            md.enable(rule)
        for rule in self.disabled:
            md.disable(rule)
        for k, v in self.options:
            md.options[k] = v
        return md

    def as_meta(self) -> dict[str, Any]:
        return {
            "preset": self.preset,
            "enabled": list(self.enabled),
            "disabled": list(self.disabled),
            "options": dict(self.options),
        }


# Exact mirrors of the configurations in tests/test_port/test_fixtures.py.
# Naming kept in sync with the fixture file basenames so the C-side tests
# can dispatch on the JSONL filename.
FIXTURE_CONFIGS: dict[str, Config] = {
    "linkify": Config(
        "linkify",
        enabled=("linkify",),
        options=(("linkify", True),),
    ),
    "smartquotes": Config(
        "smartquotes",
        enabled=("replacements", "smartquotes"),
        options=(("typographer", True),),
    ),
    "typographer": Config(
        "typographer",
        enabled=("replacements",),
        options=(("typographer", True),),
    ),
    "tables": Config(
        "tables",
        enabled=("table",),
    ),
    "commonmark_extras": Config(
        "commonmark_extras",
        preset="commonmark",
        options=(("langPrefix", ""),),
    ),
    "normalize": Config("normalize", preset="commonmark"),
    "fatal": Config(
        "fatal",
        preset="commonmark",
        enabled=("replacements",),
        options=(("typographer", True),),
    ),
    "strikethrough": Config("strikethrough", enabled=("strikethrough",)),
    "strikethrough_single_tilde": Config(
        "strikethrough_single_tilde", preset="gfm-like2"
    ),
    "disable_code_block": Config(
        "disable_code_block",
        enabled=("table",),
        disabled=("code",),
    ),
    "tasklists": Config("tasklists", preset="gfm-like2"),
    "alerts": Config("alerts", preset="gfm-like2"),
    "issue-fixes": Config("issue-fixes"),
    # Fixtures that ship in the repo but aren't exercised by pytest --
    # we still capture them under sensible defaults so the C port has
    # extra coverage of XSS / proto / punycode handling.
    "xss": Config("xss"),
    "proto": Config("proto"),
    "punycode": Config("punycode"),
    "commonmark_spec": Config("commonmark_spec", preset="commonmark"),
}


# ---------------------------------------------------------------------------
# Case sources
# ---------------------------------------------------------------------------
@dataclasses.dataclass
class Case:
    source: str
    id: int
    title: str
    config: Config
    input: str
    expected_html: str | None  # what the upstream test asserts, if any


def _read_fixture_file(path: Path) -> list[tuple[int, str, str, str]]:
    """Local copy of ``markdown_it.utils.read_fixture_file``.

    Inlined so this script is independent of the package being tested
    (handy if someone is bisecting a regression in ``utils.py``).
    """
    text = path.read_text(encoding="utf-8")
    tests: list[list[Any]] = []
    section = 0
    last_pos = 0
    lines = text.splitlines(keepends=True)
    for i, line in enumerate(lines):
        if line.rstrip() == ".":
            if section == 0:
                tests.append([i, lines[i - 1].strip()])
                section = 1
            elif section == 1:
                tests[-1].append("".join(lines[last_pos + 1 : i]))
                section = 2
            elif section == 2:
                tests[-1].append("".join(lines[last_pos + 1 : i]))
                section = 0
            last_pos = i
    return [(line, title, inp, exp) for (line, title, inp, exp) in tests]


def cases_from_commonmark_json(path: Path) -> Iterator[Case]:
    config = FIXTURE_CONFIGS["commonmark_spec"]
    data = json.loads(path.read_text(encoding="utf-8"))
    for entry in data:
        section = entry.get("section", "")
        ex = entry["example"]
        yield Case(
            source="commonmark_spec",
            id=ex,
            title=f"{section} example {ex}",
            config=config,
            input=entry["markdown"],
            expected_html=entry.get("html"),
        )


def cases_from_spec_md(path: Path) -> Iterator[Case]:
    yield Case(
        source="commonmark_spec_doc",
        id=0,
        title="Full CommonMark spec.md",
        config=FIXTURE_CONFIGS["commonmark_spec"],
        input=path.read_text(encoding="utf-8"),
        expected_html=None,
    )


def cases_from_fixture(stem: str, path: Path) -> Iterator[Case]:
    config = FIXTURE_CONFIGS.get(stem)
    if config is None:
        # Unknown fixture: fall through to vanilla MarkdownIt() as a safe default.
        config = Config(stem)
    for line, title, inp, exp in _read_fixture_file(path):
        yield Case(
            source=stem,
            id=line,
            title=title or f"{stem}:{line}",
            config=config,
            input=inp,
            expected_html=exp,
        )


def discover_cases() -> Iterator[Case]:
    yield from cases_from_commonmark_json(TESTS_ROOT / "test_cmark_spec" / "commonmark.json")
    spec_md = TESTS_ROOT / "test_cmark_spec" / "spec.md"
    if spec_md.exists():
        yield from cases_from_spec_md(spec_md)
    fixture_dir = TESTS_ROOT / "test_port" / "fixtures"
    for md_path in sorted(fixture_dir.glob("*.md")):
        yield from cases_from_fixture(md_path.stem, md_path)


# ---------------------------------------------------------------------------
# Token serialization
# ---------------------------------------------------------------------------
def _meta_serializer(meta: dict[Any, Any]) -> Any:
    """Make Token.meta JSON-safe; fall back to repr() for foreign types."""
    def encode(v: Any) -> Any:
        if isinstance(v, (str, int, float, bool)) or v is None:
            return v
        if isinstance(v, list):
            return [encode(x) for x in v]
        if isinstance(v, tuple):
            return [encode(x) for x in v]
        if isinstance(v, dict):
            return {str(k): encode(val) for k, val in v.items()}
        return repr(v)

    return encode(meta)


# Subset of MarkdownIt options that affect parsing or rendering and so
# must round-trip through the JSONL row for the C runner to apply. Any
# new option that the C port grows to consume (e.g. for new plugins)
# should be appended here so preset-derived values reach the C side.
_EFFECTIVE_OPTION_KEYS: tuple[str, ...] = (
    "html",
    "xhtmlOut",
    "breaks",
    "langPrefix",
    "linkify",
    "typographer",
    "strikethrough_single_tilde",
    "tasklists",
    "tasklists_editable",
    "alerts",
)


def _effective_options(md: Any, explicit: dict[str, Any]) -> dict[str, Any]:
    """Merge preset-derived options into the explicit overrides.

    `Config.options` only captures `options_update`-style entries, so
    flags promoted by a preset (e.g. `gfm-like2` setting
    `strikethrough_single_tilde=True`) are otherwise invisible to the C
    runner. We surface a curated subset on top of the explicit dict so
    the JSONL row carries everything the C side needs to reconstruct
    the parser configuration.
    """
    merged: dict[str, Any] = dict(explicit)
    for key in _EFFECTIVE_OPTION_KEYS:
        if key in merged:
            continue
        try:
            merged[key] = md.options[key]
        except KeyError:
            continue
    return merged


def render_case(case: Case, md_cache: dict[str, Any]) -> dict[str, Any]:
    key = case.source
    md = md_cache.get(key)
    if md is None:
        md = case.config.build()
        md_cache[key] = md

    tokens = md.parse(case.input)
    token_dicts = [
        t.as_dict(as_upstream=True, meta_serializer=_meta_serializer) for t in tokens
    ]
    html = md.render(case.input)

    meta = case.config.as_meta()
    meta["options"] = _effective_options(md, meta["options"])
    row: dict[str, Any] = {
        "source": case.source,
        "id": case.id,
        "title": case.title,
        **meta,
        "input": case.input,
        "tokens": token_dicts,
        "html": html,
    }
    if case.expected_html is not None:
        row["expected_html"] = case.expected_html
        row["html_matches_expected"] = html == case.expected_html
    return row


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------
def _hash_file(path: Path) -> str:
    h = hashlib.sha256()
    h.update(path.read_bytes())
    return h.hexdigest()


def write_oracle(
    cases: Iterable[Case],
    output_dir: Path,
    *,
    progress: Callable[[int, int], None] | None = None,
) -> dict[str, Any]:
    output_dir.mkdir(parents=True, exist_ok=True)
    md_cache: dict[str, Any] = {}
    files: dict[str, Path] = {}
    handles: dict[str, Any] = {}
    counts: dict[str, int] = {}
    mismatches: dict[str, int] = {}

    cases_list = list(cases)
    total = len(cases_list)

    try:
        for i, case in enumerate(cases_list):
            row = render_case(case, md_cache)
            src = case.source
            if src not in handles:
                fpath = output_dir / f"{src}.jsonl"
                files[src] = fpath
                handles[src] = fpath.open("w", encoding="utf-8", newline="\n")
                counts[src] = 0
                mismatches[src] = 0
            handles[src].write(json.dumps(row, ensure_ascii=False) + "\n")
            counts[src] += 1
            if "html_matches_expected" in row and not row["html_matches_expected"]:
                mismatches[src] += 1
            if progress is not None and (i % 50 == 0 or i + 1 == total):
                progress(i + 1, total)
    finally:
        for h in handles.values():
            h.close()

    markdown_it = _markdown_it_module()
    manifest = {
        "markdown_it_version": getattr(markdown_it, "__version__", "unknown"),
        "python_version": sys.version,
        "total_cases": total,
        "files": [
            {
                "source": src,
                "path": str(fpath.relative_to(output_dir)),
                "cases": counts[src],
                "html_mismatches_vs_upstream": mismatches[src],
                "sha256": _hash_file(fpath),
            }
            for src, fpath in sorted(files.items())
        ],
    }
    manifest_path = output_dir / "manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    return manifest


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def _print_progress(done: int, total: int) -> None:
    pct = (done / total * 100.0) if total else 100.0
    print(f"  [{done:>5d}/{total:>5d}]  {pct:5.1f}%", end="\r", file=sys.stderr)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=DEFAULT_OUTPUT_DIR,
        help=f"Where to write JSONL files (default: {DEFAULT_OUTPUT_DIR})",
    )
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="Suppress progress output.",
    )
    args = parser.parse_args(argv)

    cases = list(discover_cases())
    print(f"Discovered {len(cases)} test cases.", file=sys.stderr)

    progress = None if args.quiet else _print_progress
    manifest = write_oracle(cases, args.output_dir, progress=progress)
    if not args.quiet:
        print(file=sys.stderr)  # newline after the carriage-returned progress

    print(f"Wrote {manifest['total_cases']} oracle rows to {args.output_dir}/")
    for entry in manifest["files"]:
        mismatch_note = (
            f"  (html != upstream in {entry['html_mismatches_vs_upstream']})"
            if entry["html_mismatches_vs_upstream"]
            else ""
        )
        print(
            f"  {entry['path']:<40s} {entry['cases']:>5d} cases"
            f"{mismatch_note}"
        )
    print(
        f"Manifest: {(args.output_dir / 'manifest.json').relative_to(REPO_ROOT)}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
