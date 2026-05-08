"""Shared pytest harness for running the upstream Python test corpus
against the ``mdit_c`` CPython extension.

The CTest entry ``python_pytest`` invokes pytest with::

    MDIT_C_STAGE_DIR=<build>/python_stage \
        python -m pytest mdit-c/bindings/python/upstream_tests

Each test in this folder either uses the upstream test data files
directly (``tests/test_cmark_spec/commonmark.json``,
``tests/test_port/fixtures/*.md``) or reproduces a small upstream test
body verbatim, but with ``mdit_c.MarkdownIt`` substituted for
``markdown_it.MarkdownIt``. The tests are byte-for-byte parity checks
against the upstream HTML expectations, so any divergence between the
C engine and the Python reference shows up here.

If ``mdit_c`` or ``markdown_it`` (the upstream package, used for fixture
parsing utilities) cannot be imported, the entire test session
collects nothing and exits cleanly — the CTest entry treats that as a
hard failure since the build harness is supposed to provide both.
"""
from __future__ import annotations

import os
import sys
from pathlib import Path

import pytest


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[4]


def _stage_dir() -> Path | None:
    env = os.environ.get("MDIT_C_STAGE_DIR")
    if env:
        p = Path(env).resolve()
        if p.is_dir():
            return p
    # Best-effort fallback: walk up to find a build dir with python_stage.
    for ancestor in Path(__file__).resolve().parents:
        candidate = ancestor / "build" / "python_stage"
        if candidate.is_dir():
            return candidate
    return None


def _ensure_paths() -> None:
    """Move the staged extension to the front of sys.path so the
    in-tree ``mdit_c/`` source dir (which has no compiled ``.pyd``)
    cannot shadow the built artifact when pytest's import-mode adds
    the bindings parent directory to sys.path. The repo root is
    appended so the upstream ``markdown_it`` source tree remains
    importable for fixture utilities.
    """
    stage = _stage_dir()
    if stage is not None:
        s = str(stage)
        # Remove + reinsert so stage always wins over any pytest- or
        # driver-added entries.
        sys.path[:] = [p for p in sys.path if p != s]
        sys.path.insert(0, s)
        # Also drop any entry that exposes the *source* `mdit_c` dir,
        # which is missing the compiled extension. The bindings parent
        # is added by pytest's import-mode to support our conftest.py.
        bindings_parent = str(Path(__file__).resolve().parent.parent)
        sys.path[:] = [p for p in sys.path if p != bindings_parent]
    root = _repo_root()
    if str(root) not in sys.path:
        sys.path.append(str(root))


_ensure_paths()


def pytest_collection_modifyitems(config, items):
    """Skip everything if the staged extension isn't importable —
    keeps the test session stable on machines where Python wasn't found
    at CMake configure time.
    """
    try:
        import mdit_c  # noqa: F401
    except ImportError as exc:
        print(f"\n[mdit-c upstream-tests] SKIP — mdit_c not importable: "
              f"{exc!r}",
              flush=True)
        skip = pytest.mark.skip(reason=f"mdit_c not importable: {exc!r}")
        for item in items:
            item.add_marker(skip)
        return
    try:
        import markdown_it  # noqa: F401
    except ImportError as exc:
        print(f"\n[mdit-c upstream-tests] SKIP — markdown_it not importable: "
              f"{exc!r}",
              flush=True)
        skip = pytest.mark.skip(
            reason=f"markdown_it (upstream) not importable: {exc!r}")
        for item in items:
            item.add_marker(skip)


@pytest.fixture(scope="session")
def repo_root() -> Path:
    return _repo_root()
