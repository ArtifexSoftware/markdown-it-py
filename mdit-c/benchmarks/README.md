# mdit-c benchmarks

A small Python harness that runs the same Markdown corpus through
three engines and reports throughput:

| Engine | How it's invoked | Why we measure it |
| ------ | ---------------- | ----------------- |
| `markdown-it-py` | in-process via `import markdown_it` | upstream baseline; the C engine has to beat this to justify existing |
| `mdit-c`         | subprocess: `markdown-it-c --stdin` | the in-tree C engine; uses the shipping CLI so process startup is included (and amortised over a multi-KB corpus) |
| `cmark`          | subprocess: `cmark` | reference C implementation; honesty check |

Inputs come from the upstream `benchmarking/samples/` corpus
(headings, lists, tables, blockquote-heavy, em-worst-case, …),
concatenated with blank-line separators so the parsers don't fuse
adjacent fixtures into one block.

## Quick start

Build the C engine + CLI, then run the script:

```sh
cmake -S mdit-c -B mdit-c/build -DCMAKE_BUILD_TYPE=Release
cmake --build mdit-c/build --config Release
python mdit-c/benchmarks/bench_engines.py
```

Sample output (Linux / Release):

```
corpus: 27 files, 84,932 bytes total
iters:  20

engine          version        best (ms)    MB/s   vs baseline
--------------  -------------  -----------  -----  -----------
markdown-it-py  3.0.0             65.34     1.30   1.00x
mdit-c          subprocess         8.71     9.75   7.50x
cmark           subprocess         3.42    24.84  19.10x
```

(`mdit-c` includes process-start cost; for an in-process apples-to-
apples comparison, link the static library directly. The CPython
extension under `mdit-c/bindings/python/` is what end-users will pay
the much-smaller startup cost on.)

## Options

- `--iters N` — number of timing iterations per engine (default 20).
  The script reports the **minimum** wall-clock per `timeit.repeat`
  convention so the result reflects best-case throughput rather than
  noisy averages.
- `--mdit-c PATH` — explicit path to the `markdown-it-c` binary.
  Falls back to `$MDIT_C_BIN`, then auto-detection under
  `mdit-c/build*/`, then `$PATH`.
- `--baseline ENGINE` — engine name to use as the `vs baseline`
  reference column (default `markdown-it-py`).
- `--json results.json` — additionally dump the raw measurements as
  JSON for plotting / regression tracking.

Engines that aren't available are reported as `skipped` and the
script exits 0; missing `cmark` doesn't fail CI.

## Caveats

- **Subprocess overhead.** Each `mdit-c` and `cmark` measurement
  pays the cost of forking + execing the binary. We amortise this by
  feeding the entire corpus on stdin in a single invocation, but it
  still skews against the C engines for very small inputs. For a
  measurement that doesn't pay startup cost, run the upstream
  `benchmarking/bench_packages.py` instead and add the in-tree
  CPython extension once `mdit_c` is on `pip install`.
- **Build configuration matters.** Configure the C engine with
  `-DCMAKE_BUILD_TYPE=Release` (or `RelWithDebInfo`) before
  benchmarking. Sanitizer-instrumented builds will look 3–10x
  slower than the truth.
