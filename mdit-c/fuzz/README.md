# mdit-c fuzz harnesses

libFuzzer-style entrypoints that stress the parse, render, and
token-serialisation paths. Mirrors the upstream Python fuzzers in
`tests/fuzz/` (`fuzz_markdown.py`, `fuzz_markdown_extended.py`).

## Harnesses

| Binary | Surface | Mirrors upstream |
| ------ | ------- | ---------------- |
| `fuzz_parse_render`    | `mdit_md_render` (full pipeline)         | `fuzz_markdown.py` |
| `fuzz_inline`          | `mdit_md_render_inline` (inline-only)    | inline-only slice of the extended fuzzer |
| `fuzz_token_roundtrip` | `mdit_md_parse` + `mdit_tokens_to_json`  | the JSON oracle contract used by `tests/test_token_oracle` |

Each harness defines `LLVMFuzzerTestOneInput(const uint8_t *, size_t)`
— the standard libFuzzer entrypoint. Two link modes are supported:

### Coverage-guided fuzzing (Clang)

```sh
cmake -S mdit-c -B build-fuzz \
      -DMDIT_BUILD_FUZZ=ON -DMDIT_FUZZ_LIBFUZZER=ON \
      -DCMAKE_C_COMPILER=clang
cmake --build build-fuzz
./build-fuzz/fuzz/fuzz_parse_render mdit-c/fuzz/corpus/parse_render
```

`MDIT_FUZZ_LIBFUZZER=ON` links the harness with
`-fsanitize=fuzzer,address,undefined` so libFuzzer supplies `main`,
ASan reports memory bugs, and UBSan catches undefined behaviour. This
is the build mode oss-fuzz consumes via the upstream
`projects/markdown-it-py/build.sh` recipe.

### Replay smoke (any C99 compiler — the default)

```sh
cmake -S mdit-c -B build -DMDIT_BUILD_FUZZ=ON
cmake --build build
ctest --test-dir build -L fuzz
```

When `MDIT_FUZZ_LIBFUZZER=OFF`, each harness links the standalone
`fuzz_main.c` driver, which iterates over a directory of regression
inputs and replays each through `LLVMFuzzerTestOneInput`. This is the
shape used by the `fuzz_smoke_*` CTest entries: small, deterministic,
runs in well under a second per harness, and works with MSVC / MinGW
gcc / vanilla clang. Inputs that have crashed the engine in the past
get checked into `corpus/<harness>/` so they stay green on every CI
run.

### Reproducing an oss-fuzz finding

If an oss-fuzz run reports a crash on `fuzz_parse_render` with a
minimised testcase file `crash-XYZ`:

```sh
# Coverage-guided rebuild (gives you ASan + UBSan stack traces)
cmake --build build-fuzz
./build-fuzz/fuzz/fuzz_parse_render crash-XYZ
```

The replay binary built without libFuzzer can also do quick triage:

```sh
./build/fuzz/fuzz_parse_render crash-XYZ
```

Once fixed, drop a (possibly shrunk) copy of the testcase into
`corpus/parse_render/` so the regression sticks.

## Corpus

Inputs are seeded from common Markdown shapes (headings, lists,
tables, references, HTML blocks, fenced code, escapes, emoji). They
are intentionally short — coverage growth happens during real
fuzzing; the in-tree corpus exists to catch obvious regressions
deterministically.
