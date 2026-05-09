#!/usr/bin/env python3
"""
bench_engines.py — wall-clock comparison of three Markdown engines
running over the same corpus.

Engines:

  * ``mdit-c``        — the in-tree C engine, exercised through the
                        ``markdown-it-c`` CLI subprocess. Pulls double
                        duty: confirms there's no embarrassing perf
                        regression and validates the CLI startup is
                        amortised correctly when input is streamed
                        from stdin.
  * ``markdown-it-py``— the upstream pure-Python implementation, used
                        in-process for the lowest possible overhead.
                        The C engine has to beat *this* to justify
                        existing.
  * ``cmark``         — reference C implementation; an honesty check.
                        Skipped if the binary isn't on PATH.

Methodology:

  1. Concatenate every ``*.md`` / ``*.txt`` sample in the upstream
     ``benchmarking/samples/`` corpus into a single fixed input. Most
     individual fixtures are <1KB; concatenation gives us ~tens of KB,
     enough that subprocess startup is in the noise rather than the
     signal.
  2. For each engine, render the combined input N times back-to-back
     and report the *minimum* wall-clock time (per the
     ``timeit.repeat`` convention: low = least system noise, not
     average behaviour).
  3. Print throughput in MB/s plus a relative speedup factor versus
     ``markdown-it-py``.

Usage:

    python mdit-c/benchmarks/bench_engines.py
    python mdit-c/benchmarks/bench_engines.py --iters 50
    python mdit-c/benchmarks/bench_engines.py --mdit-c path/to/markdown-it-c
    python mdit-c/benchmarks/bench_engines.py --json results.json

By default we look for ``markdown-it-c`` (or ``markdown-it-c.exe`` on
Windows) under ``mdit-c/build*/bin/`` next to this script and on
``$PATH``. Override with ``--mdit-c <path>`` or the ``MDIT_C_BIN``
environment variable. The script exits 0 even if some engines are
unavailable — the output flags them as ``skipped`` rather than
failing CI.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent.parent
SAMPLES = REPO_ROOT / "benchmarking" / "samples"


def find_mdit_c_bin(override: str | None) -> str | None:
    """Locate the ``markdown-it-c`` binary.

    Resolution order:

    1. Explicit ``--mdit-c`` argument.
    2. ``$MDIT_C_BIN`` environment variable.
    3. ``markdown-it-c[.exe]`` under ``mdit-c/build*/bin/`` (any
       config sub-directory; multi-config generators put binaries
       under ``Debug/`` / ``RelWithDebInfo/`` which is also covered).
    4. ``markdown-it-c`` on ``$PATH``.
    """
    if override:
        p = Path(override)
        return str(p) if p.is_file() else None

    env = os.environ.get("MDIT_C_BIN")
    if env:
        p = Path(env)
        if p.is_file():
            return str(p)

    exe = "markdown-it-c.exe" if os.name == "nt" else "markdown-it-c"
    for build_dir in sorted((REPO_ROOT / "mdit-c").glob("build*")):
        for candidate in build_dir.rglob(exe):
            return str(candidate)

    return shutil.which("markdown-it-c")


def find_cmark_bin() -> str | None:
    return shutil.which("cmark")


def load_corpus() -> tuple[bytes, str]:
    """Return ``(combined_bytes, summary_string)``.

    The combined buffer is what's fed to every engine. Joining every
    sample with two blank lines preserves CommonMark block boundaries
    so the parsers don't accidentally fuse adjacent fixtures into one
    block.
    """
    if not SAMPLES.is_dir():
        raise SystemExit(f"corpus not found: {SAMPLES}")
    chunks: list[bytes] = []
    sizes: list[tuple[str, int]] = []
    for path in sorted(SAMPLES.iterdir()):
        if path.suffix.lower() not in (".md", ".txt"):
            continue
        data = path.read_bytes()
        chunks.append(data)
        sizes.append((path.name, len(data)))
    combined = b"\n\n".join(chunks)
    summary = f"{len(sizes)} files, {len(combined):,} bytes total"
    return combined, summary


def time_call(fn, iters: int) -> float:
    """Run ``fn()`` ``iters`` times; return the minimum wall-clock."""
    best = float("inf")
    for _ in range(iters):
        t0 = time.perf_counter()
        fn()
        dt = time.perf_counter() - t0
        if dt < best:
            best = dt
    return best


def bench_markdown_it_py(payload_bytes: bytes, iters: int) -> dict | None:
    try:
        import markdown_it
    except ImportError:
        return None
    md = markdown_it.MarkdownIt("commonmark")
    text = payload_bytes.decode("utf-8")
    # One warm-up render so byte-cache effects don't penalise the first
    # measurement; matters most for the JIT-free CPython path here.
    md.render(text)
    secs = time_call(lambda: md.render(text), iters)
    return {
        "engine": "markdown-it-py",
        "version": getattr(markdown_it, "__version__", "unknown"),
        "best_seconds": secs,
        "throughput_mb_per_s": (len(payload_bytes) / 1e6) / secs,
    }


def bench_subprocess(name: str, argv: list[str], payload: bytes,
                     iters: int) -> dict:
    # Warm-up + a sanity pass that the binary actually accepts the
    # input. If it errors, abort with a clear message instead of
    # publishing a misleading "0.001s" reading.
    proc = subprocess.run(argv, input=payload, capture_output=True)
    if proc.returncode != 0:
        raise SystemExit(
            f"{name}: warm-up run failed with exit code {proc.returncode}\n"
            f"stderr:\n{proc.stderr.decode('utf-8', errors='replace')}"
        )
    secs = time_call(
        lambda: subprocess.run(argv, input=payload, capture_output=True,
                               check=True),
        iters,
    )
    return {
        "engine": name,
        "version": "subprocess",
        "best_seconds": secs,
        "throughput_mb_per_s": (len(payload) / 1e6) / secs,
        "argv": argv,
    }


def bench_mdit_c(bin_path: str, payload: bytes, iters: int) -> dict:
    return bench_subprocess(
        "mdit-c",
        [bin_path, "--stdin"],
        payload,
        iters,
    )


def bench_cmark(bin_path: str, payload: bytes, iters: int) -> dict:
    return bench_subprocess(
        "cmark",
        [bin_path],
        payload,
        iters,
    )


def render_table(rows: list[dict], baseline: str) -> str:
    if not rows:
        return "(no engines available)"
    base_secs = next(
        (r["best_seconds"] for r in rows if r["engine"] == baseline),
        None,
    )
    headers = ["engine", "version", "best (ms)", "MB/s", "vs baseline"]
    out_rows = []
    for r in rows:
        ms = r["best_seconds"] * 1000
        mbps = r["throughput_mb_per_s"]
        if base_secs is None or r["engine"] == baseline:
            ratio = "1.00x" if r["engine"] == baseline else "—"
        else:
            ratio = f"{base_secs / r['best_seconds']:.2f}x"
        out_rows.append([
            r["engine"],
            r["version"],
            f"{ms:8.2f}",
            f"{mbps:7.2f}",
            ratio,
        ])
    widths = [max(len(h), max(len(row[i]) for row in out_rows))
              for i, h in enumerate(headers)]
    lines = []
    fmt = "  ".join("{:<" + str(w) + "}" for w in widths)
    lines.append(fmt.format(*headers))
    lines.append(fmt.format(*("-" * w for w in widths)))
    for row in out_rows:
        lines.append(fmt.format(*row))
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iters", type=int, default=20,
                    help="Iterations per engine (default: 20).")
    ap.add_argument("--mdit-c", default=None,
                    help="Path to the markdown-it-c binary.")
    ap.add_argument("--baseline", default="markdown-it-py",
                    help="Engine to use as the relative-speed baseline.")
    ap.add_argument("--json", default=None, type=Path,
                    help="Also write the raw results to this JSON file.")
    args = ap.parse_args()

    payload, summary = load_corpus()
    print(f"corpus: {summary}")
    print(f"iters:  {args.iters}")
    print()

    rows: list[dict] = []

    py = bench_markdown_it_py(payload, args.iters)
    if py is not None:
        rows.append(py)
    else:
        print("markdown-it-py: skipped (package not importable)")

    bin_mdit = find_mdit_c_bin(args.mdit_c)
    if bin_mdit:
        rows.append(bench_mdit_c(bin_mdit, payload, args.iters))
    else:
        print("mdit-c: skipped (markdown-it-c binary not found; "
              "build with `cmake --build mdit-c/build`)")

    bin_cmark = find_cmark_bin()
    if bin_cmark:
        rows.append(bench_cmark(bin_cmark, payload, args.iters))
    else:
        print("cmark:  skipped (no `cmark` on PATH)")

    print()
    print(render_table(rows, args.baseline))

    if args.json:
        args.json.write_text(json.dumps(
            {"corpus": summary, "iters": args.iters, "results": rows},
            indent=2,
        ))
        print(f"\nresults written to {args.json}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
