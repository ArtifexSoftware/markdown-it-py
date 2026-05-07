#!/usr/bin/env python3
"""
regex_inventory.py — catalogue every regex used by ``markdown_it``.

This is a Phase-0 tool for the C port. It walks the Python source via
:mod:`ast`, finds calls to ``re.compile`` / ``re.match`` / ``re.search``
/ ``re.sub`` / ``re.findall`` / ``re.finditer`` / ``re.split`` /
``re.fullmatch``, attempts to recover the literal pattern (handles
string concatenation and module-level constants), and writes a Markdown
report to ``mdit-c/docs/regex-inventory.md``.

For each pattern it records:

* file:line of the call site,
* the function/class qualname containing the call,
* the literal pattern string (or ``<dynamic>`` if it can't be reduced),
* the flags passed (e.g. ``re.MULTILINE``, ``re.IGNORECASE``),
* a feature breakdown: anchors, char classes, predefined classes,
  Unicode-property classes, lookarounds, backrefs, named groups,
  non-greedy quantifiers, inline flags, alternation, recursion (n/a).

The feature breakdown is what tells us whether the pattern is amenable
to a straight hand-scanner port or whether the C side needs a real
matcher.
"""

from __future__ import annotations

import argparse
import ast
import dataclasses
import re
import sys
from collections import Counter
from collections.abc import Iterable, Iterator
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SRC_ROOT = REPO_ROOT / "markdown_it"
DEFAULT_OUTPUT = REPO_ROOT / "mdit-c" / "docs" / "regex-inventory.md"

RE_FUNCS = {
    "compile",
    "match",
    "fullmatch",
    "search",
    "sub",
    "subn",
    "split",
    "findall",
    "finditer",
}


# ---------------------------------------------------------------------------
# Feature analysis
# ---------------------------------------------------------------------------
@dataclasses.dataclass(frozen=True)
class Features:
    anchors: bool = False
    char_classes: bool = False
    predefined_classes: bool = False  # \w \W \s \S \d \D
    unicode_property: bool = False  # \p{...} -- not in Python re, but flag if seen
    lookarounds: bool = False
    backrefs: bool = False
    named_groups: bool = False
    nongreedy: bool = False
    inline_flags: bool = False
    alternation: bool = False
    case_insensitive: bool = False  # via flag or inline
    multiline: bool = False
    dotall: bool = False
    verbose: bool = False
    unicode_flag: bool = False
    ascii_flag: bool = False

    def as_tags(self) -> list[str]:
        tags: list[str] = []
        for f in dataclasses.fields(self):
            if getattr(self, f.name):
                tags.append(f.name)
        return tags


_FEATURE_PATTERNS = [
    ("anchors", re.compile(r"(?<!\\)(\^|\$|\\A|\\Z|\\b|\\B)")),
    ("char_classes", re.compile(r"(?<!\\)\[")),
    ("predefined_classes", re.compile(r"\\[wWsSdD]")),
    ("unicode_property", re.compile(r"\\p\{")),
    ("lookarounds", re.compile(r"\(\?[=!]|\(\?<[=!]")),
    ("backrefs", re.compile(r"(?<!\\)\\[1-9]|\(\?P=")),
    ("named_groups", re.compile(r"\(\?P<")),
    ("nongreedy", re.compile(r"[*+?}]\?")),
    ("inline_flags", re.compile(r"\(\?[aiLmsux]+[:)]")),
    ("alternation", re.compile(r"(?<!\\)\|")),
]


def analyse_pattern(pattern: str, flag_str: str) -> Features:
    kw: dict[str, bool] = {}
    for name, rx in _FEATURE_PATTERNS:
        if rx.search(pattern):
            kw[name] = True

    # Inline flag introspection.
    inline = re.findall(r"\(\?([aiLmsux]+)[:)]", pattern)
    inline_letters = "".join(inline)
    if "i" in inline_letters:
        kw["case_insensitive"] = True
    if "m" in inline_letters:
        kw["multiline"] = True
    if "s" in inline_letters:
        kw["dotall"] = True
    if "x" in inline_letters:
        kw["verbose"] = True
    if "a" in inline_letters:
        kw["ascii_flag"] = True
    if "u" in inline_letters:
        kw["unicode_flag"] = True

    # Module-flag introspection (textual; we don't import the modules).
    if flag_str:
        f = flag_str.replace(" ", "")
        if "IGNORECASE" in f or re.search(r"\bre\.I\b", f):
            kw["case_insensitive"] = True
        if "MULTILINE" in f or re.search(r"\bre\.M\b", f):
            kw["multiline"] = True
        if "DOTALL" in f or re.search(r"\bre\.S\b", f):
            kw["dotall"] = True
        if "VERBOSE" in f or re.search(r"\bre\.X\b", f):
            kw["verbose"] = True
        if "UNICODE" in f or re.search(r"\bre\.U\b", f):
            kw["unicode_flag"] = True
        if "ASCII" in f or re.search(r"\bre\.A\b", f):
            kw["ascii_flag"] = True

    return Features(**kw)


# ---------------------------------------------------------------------------
# AST walking
# ---------------------------------------------------------------------------
@dataclasses.dataclass
class Hit:
    file: Path
    lineno: int
    qualname: str
    func: str  # "compile", "match", ...
    pattern: str | None  # None if dynamic
    flags: str  # textual representation of the flags arg, may be ""
    features: Features | None


def _qualname_stack(node: ast.AST, parents: list[ast.AST]) -> str:
    parts: list[str] = []
    for p in parents:
        if isinstance(p, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            parts.append(p.name)
    return ".".join(parts) if parts else "<module>"


def _eval_string(node: ast.AST, consts: dict[str, str]) -> str | None:
    """Try to fold *node* into a string literal."""
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    if isinstance(node, ast.JoinedStr):
        # f-string -- only ok if every part is a constant
        out: list[str] = []
        for v in node.values:
            if isinstance(v, ast.Constant) and isinstance(v.value, str):
                out.append(v.value)
            else:
                return None
        return "".join(out)
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
        left = _eval_string(node.left, consts)
        right = _eval_string(node.right, consts)
        if left is None or right is None:
            return None
        return left + right
    if isinstance(node, ast.Name):
        return consts.get(node.id)
    if isinstance(node, ast.Attribute):
        # support things like SOME.MODULE.NAME = "..."
        return None
    return None


def _flag_text(node: ast.AST | None, src_lines: list[str]) -> str:
    if node is None:
        return ""
    try:
        return ast.get_source_segment("\n".join(src_lines) + "\n", node) or ""
    except Exception:
        return ""


def _collect_module_constants(tree: ast.Module) -> dict[str, str]:
    """Pick up top-level ``NAME = "..."`` (and concatenations of literals)."""
    consts: dict[str, str] = {}
    for stmt in tree.body:
        if isinstance(stmt, ast.Assign) and len(stmt.targets) == 1:
            tgt = stmt.targets[0]
            if isinstance(tgt, ast.Name):
                val = _eval_string(stmt.value, consts)
                if val is not None:
                    consts[tgt.id] = val
        elif isinstance(stmt, ast.AnnAssign) and isinstance(stmt.target, ast.Name):
            if stmt.value is not None:
                val = _eval_string(stmt.value, consts)
                if val is not None:
                    consts[stmt.target.id] = val
    return consts


def _is_re_call(node: ast.Call) -> tuple[bool, str]:
    """Return (is_re_call, func_name)."""
    f = node.func
    if isinstance(f, ast.Attribute) and f.attr in RE_FUNCS:
        # Match either re.<func> or some_compiled.<func> -- we only care
        # about the ones that take a *pattern* argument, i.e. `re.X(...)`
        if isinstance(f.value, ast.Name) and f.value.id == "re":
            return True, f.attr
    if isinstance(f, ast.Name) and f.id in {"compile"}:
        # `from re import compile as ... ` -- rare; ignore for now.
        return False, ""
    return False, ""


def walk_file(path: Path) -> Iterator[Hit]:
    text = path.read_text(encoding="utf-8")
    tree = ast.parse(text, filename=str(path))
    consts = _collect_module_constants(tree)
    src_lines = text.splitlines()

    parents: list[ast.AST] = []

    def visit(node: ast.AST) -> Iterator[Hit]:
        parents.append(node)
        try:
            if isinstance(node, ast.Call):
                hit, fname = _is_re_call(node)
                if hit:
                    pat_node = node.args[0] if node.args else None
                    pattern = _eval_string(pat_node, consts) if pat_node else None
                    # flags: re.compile(pattern, flags)
                    flag_node = node.args[1] if len(node.args) >= 2 else None
                    if flag_node is None:
                        for kw in node.keywords:
                            if kw.arg == "flags":
                                flag_node = kw.value
                                break
                    flags = _flag_text(flag_node, src_lines)
                    feats = (
                        analyse_pattern(pattern, flags)
                        if pattern is not None
                        else None
                    )
                    yield Hit(
                        file=path,
                        lineno=node.lineno,
                        qualname=_qualname_stack(node, parents[:-1]),
                        func=fname,
                        pattern=pattern,
                        flags=flags,
                        features=feats,
                    )
            for child in ast.iter_child_nodes(node):
                yield from visit(child)
        finally:
            parents.pop()

    yield from visit(tree)


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------
def _fmt_pattern(p: str | None) -> str:
    if p is None:
        return "_(dynamic)_"
    # Render as a fenced code block but escape backticks inside patterns
    # by switching the fence size.
    fence = "```"
    while fence in p:
        fence += "`"
    return f"{fence}\n{p}\n{fence}"


def write_report(hits: list[Hit], output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    feature_counter: Counter[str] = Counter()
    func_counter: Counter[str] = Counter()
    file_counter: Counter[str] = Counter()
    dynamic = 0

    for h in hits:
        func_counter[h.func] += 1
        rel = h.file.relative_to(REPO_ROOT).as_posix()
        file_counter[rel] += 1
        if h.features is None:
            dynamic += 1
        else:
            for tag in h.features.as_tags():
                feature_counter[tag] += 1

    lines: list[str] = []
    lines.append("# Regex inventory — `markdown_it`\n")
    lines.append(
        "_Generated by `mdit-c/scripts/regex_inventory.py`. Re-run after "
        "any source change._\n"
    )
    lines.append(f"- **Total call sites:** {len(hits)}")
    lines.append(f"- **Dynamic patterns (literal not recoverable):** {dynamic}")
    if hits:
        sample = hits[0]
        lines.append(f"- **Source root:** `{SRC_ROOT.relative_to(REPO_ROOT).as_posix()}`")
        del sample
    lines.append("")

    lines.append("## By function\n")
    lines.append("| Function | Count |")
    lines.append("| --- | ---: |")
    for k in sorted(func_counter):
        lines.append(f"| `re.{k}` | {func_counter[k]} |")
    lines.append("")

    lines.append("## By feature (cumulative across all patterns)\n")
    lines.append("| Feature | Count |")
    lines.append("| --- | ---: |")
    for tag, n in sorted(feature_counter.items(), key=lambda kv: (-kv[1], kv[0])):
        lines.append(f"| `{tag}` | {n} |")
    lines.append("")

    lines.append("## By file\n")
    lines.append("| File | Count |")
    lines.append("| --- | ---: |")
    for f, n in sorted(file_counter.items(), key=lambda kv: (-kv[1], kv[0])):
        lines.append(f"| `{f}` | {n} |")
    lines.append("")

    lines.append("## All call sites\n")
    last_file: str | None = None
    for h in sorted(hits, key=lambda x: (x.file.as_posix(), x.lineno)):
        rel = h.file.relative_to(REPO_ROOT).as_posix()
        if rel != last_file:
            lines.append(f"### `{rel}`\n")
            last_file = rel
        feat = (
            ", ".join(f"`{t}`" for t in h.features.as_tags())
            if h.features is not None
            else "_unknown_"
        )
        flags = f"`{h.flags}`" if h.flags else "—"
        lines.append(
            f"#### {rel}:{h.lineno} — `re.{h.func}` "
            f"in `{h.qualname}`"
        )
        lines.append("")
        lines.append(f"- **Flags:** {flags}")
        lines.append(f"- **Features:** {feat}")
        lines.append("")
        lines.append(_fmt_pattern(h.pattern))
        lines.append("")

    output.write_text("\n".join(lines), encoding="utf-8")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def iter_python_files(root: Path) -> Iterable[Path]:
    for p in sorted(root.rglob("*.py")):
        if "__pycache__" in p.parts:
            continue
        yield p


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source",
        type=Path,
        default=SRC_ROOT,
        help=f"Python source root to scan (default: {SRC_ROOT})",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=DEFAULT_OUTPUT,
        help=f"Markdown report output path (default: {DEFAULT_OUTPUT})",
    )
    args = parser.parse_args(argv)

    hits: list[Hit] = []
    for path in iter_python_files(args.source):
        try:
            hits.extend(walk_file(path))
        except SyntaxError as exc:
            print(f"warning: skipping {path}: {exc}", file=sys.stderr)

    write_report(hits, args.output)
    print(
        f"Wrote {args.output.relative_to(REPO_ROOT).as_posix()} "
        f"({len(hits)} regex call sites)."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
