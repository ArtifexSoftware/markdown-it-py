"""Driver for the ``python_pytest`` CTest entry.

Tries to import ``pytest`` and ``markdown_it``; if either is missing,
prints a SKIP marker and exits 0 so a Python-less / dependency-light
build environment doesn't fail the test suite. Otherwise it forwards
to ``pytest.main`` against ``upstream_tests``.

Usage::

    python run_upstream_pytest.py <stage_dir>

``stage_dir`` is the path that contains the ``mdit_c`` package; the
driver exports it as ``MDIT_C_STAGE_DIR`` for the conftest.
"""
from __future__ import annotations

import os
import sys
from pathlib import Path


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print("usage: run_upstream_pytest.py <stage_dir>", file=sys.stderr)
        return 64

    stage = Path(argv[1]).resolve()
    if not stage.is_dir():
        print(f"upstream-pytest: stage dir not found: {stage}", file=sys.stderr)
        return 2
    os.environ["MDIT_C_STAGE_DIR"] = str(stage)

    # Make the in-tree markdown_it package importable for local
    # developer runs where the package hasn't been ``pip install``-ed
    # into the active interpreter.
    repo_root = Path(__file__).resolve().parents[3]
    if str(repo_root) not in sys.path:
        sys.path.insert(0, str(repo_root))
    pp = os.environ.get("PYTHONPATH", "")
    parts = pp.split(os.pathsep) if pp else []
    if str(repo_root) not in parts and str(stage) not in parts:
        os.environ["PYTHONPATH"] = os.pathsep.join(
            [str(repo_root), str(stage), *parts])

    try:
        import pytest  # noqa: F401
    except ImportError as exc:  # pragma: no cover
        print(f"upstream-pytest: SKIP — pytest not importable ({exc!r})")
        return 0

    try:
        import markdown_it  # noqa: F401
    except ImportError as exc:  # pragma: no cover
        print(f"upstream-pytest: SKIP — markdown_it not importable ({exc!r})")
        return 0

    here = Path(__file__).resolve().parent
    upstream_tests = here / "upstream_tests"

    args = [
        "-p", "no:cacheprovider",
        "--rootdir", str(upstream_tests),
        "--import-mode=importlib",
        "--tb=short",
        "-q",
        str(upstream_tests),
    ]
    return int(pytest.main(args))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
