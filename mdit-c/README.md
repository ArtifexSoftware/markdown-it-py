# mdit-c

A C11 port of [markdown-it-py](../markdown_it/) — itself a Python port of
[markdown-it](https://github.com/markdown-it/markdown-it).

This directory holds the C implementation, a CLI, and an optional CPython
extension that exposes the same Python API as the original `markdown_it`
package (used as a continuous correctness oracle during the port).

> Status: **Phase 0** (discovery & test harness). No engine code is committed
> yet; see [`docs/PORT_PLAN.md`](docs/PORT_PLAN.md) for the multi-phase plan.

## Design choices (defaults locked in for v1)

| Area              | Choice                                                         |
| ----------------- | -------------------------------------------------------------- |
| Public surface    | C library + CLI + optional CPython extension                   |
| C standard        | C11, no compiler-specific extensions in public headers         |
| String encoding   | UTF-8 in/out; codepoint helpers for classification             |
| Regex strategy    | Hand-written scanners; minimal NFA matcher only if forced      |
| Memory model      | Per-parse arena allocator; explicit ownership at the API edge  |
| Build             | CMake (≥ 3.20), targets: static + shared lib, CLI, py-ext      |
| Plugin API        | Mirrors `Ruler.before/after/at` and `MarkdownIt.use` 1:1       |
| License           | MIT (matches upstream)                                         |

See [`docs/PORT_PLAN.md`](docs/PORT_PLAN.md) for full rationale and the
phase-by-phase delivery plan.

## Layout

```text
mdit-c/
  CMakeLists.txt          # top-level build (Phase 0: skeleton only)
  include/mdit/           # public headers
  src/                    # implementation (filled in Phase 1+)
  cli/                    # md_cli command-line tool (Phase 5)
  bindings/python/        # optional CPython extension (Phase 5)
  tests/                  # C tests, oracle data, fuzz harnesses
    oracle/               # JSONL token/HTML oracle generated from Python
  scripts/                # tooling that runs against the Python source
    regex_inventory.py    # catalogues every regex used by markdown_it
    token_oracle.py       # dumps tokens+html for the entire test corpus
  docs/                   # plan, regex inventory, porting notes
```

## Phase 0 quickstart

From the repo root, with the project installed in a venv (e.g.
`.venv-port`):

```bash
# 1. Catalogue every regex in markdown_it/ and write a Markdown report.
.venv-port/Scripts/python mdit-c/scripts/regex_inventory.py

# 2. Generate the token/HTML oracle (the "golden" corpus the C port
#    must match byte-for-byte).
.venv-port/Scripts/python mdit-c/scripts/token_oracle.py
```

The first run produces `mdit-c/docs/regex-inventory.md`; the second
populates `mdit-c/tests/oracle/` with one JSONL file per fixture source
plus a manifest. Both commands are idempotent.
