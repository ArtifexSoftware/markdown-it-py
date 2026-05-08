# mdit-c Port Plan

This is the working plan agreed at the start of the port. It mirrors the
plan that was approved in chat so it lives next to the code instead of
in agent transcripts.

## 0. Locked-in defaults

| Area              | Choice                                                         |
| ----------------- | -------------------------------------------------------------- |
| Public surface    | C library + CLI **and** an optional CPython extension          |
| C standard        | C11, no compiler-specific extensions in public headers         |
| String encoding   | UTF-8 in/out, codepoint helpers for classification             |
| Regex strategy    | Hand scanners; tiny dependency-free matcher only if forced     |
| Memory model      | Per-parse arena allocator                                      |
| Build             | CMake (≥ 3.20)                                                 |
| Plugin API        | Mirrors `Ruler.before/after/at` and `MarkdownIt.use` 1:1       |
| License           | MIT                                                            |

## 1. Source-of-truth corpus

The Python suite is the oracle. Two test bodies anchor the port:

- `tests/test_cmark_spec/commonmark.json` — ~700 CommonMark spec examples
  (markdown → html pairs).
- `tests/test_port/fixtures/*.md` — fixture-style cases (title / input /
  expected) parsed by `markdown_it.utils.read_fixture_file`.

`scripts/token_oracle.py` runs the *current Python implementation* over
this entire corpus and writes one JSONL row per case to
`tests/oracle/`. Each row contains:

```json
{
  "source": "commonmark_spec",
  "id": 12,
  "title": "Tabs example 12",
  "preset": "commonmark",
  "options": {"langPrefix": "language-"},
  "input": "...",
  "tokens": [ /* md.parse(input) flattened with as_dict() */ ],
  "html":   "..."
}
```

The C port then has a deterministic, byte-for-byte target.

## 2. Phases

### Phase 0 — discovery & harness *(in progress)*

- [x] Project skeleton: `CMakeLists.txt`, `include/mdit/mdit.h`,
      directory tree, `.gitignore`, README, this plan doc.
- [ ] `scripts/regex_inventory.py` → `docs/regex-inventory.md` cataloguing
      every `re.compile`/`re.match`/`re.search`/`re.sub` site, with
      pattern, flags, and feature breakdown (anchors, char classes,
      Unicode props, lookarounds, backrefs, named groups).
- [ ] `scripts/token_oracle.py` → `tests/oracle/*.jsonl` covering the
      whole test corpus.
- [ ] CI matrix (Linux/macOS/Windows × GCC/Clang/MSVC) running
      `cmake -S . -B build` as a configure-only smoke build, plus an
      ASan/UBSan job for when source lands.

### Phase 1 — foundations

Order chosen so each layer can be unit-tested in isolation.

- [x] **`arena.{h,c}`** — bump allocator with growable chunks, alignment
      knob, in-place try-extend, reset, OOM hook. 11 tests, MSVC `/W4 /WX`
      clean.
- [x] **`str.{h,c}`** — UTF-8 view, lossy + strict codepoint decode,
      encoder, `is_utf8`, `count_codepoints`, ASCII classifiers, the
      CommonMark ASCII-punct table. 16 tests.
- [x] **`vec.{h,c}`** — typed dynamic arrays via macros, arena-backed
      with arena `try_extend` fast path, malloc-backed with realloc.
      5 tests.
- [x] **`map.{h,c}`** — small ordered string→`mdit_value` map (insertion
      order preserved, matching upstream `attrs` serialisation), value
      union covers null/bool/int/double/str. 5 tests.
- [x] **Test harness** — `tests/mdit_test.h` + `mdit_test_main.h`,
      header-only, MSVC-clean assertions, ctest-friendly (one suite per
      `.c` file, runs in parallel under `ctest -j`).
- [x] **`utf.{h,c}`** — Unicode classification. `mdit_is_punct` is
      driven by a generated 2-level bitmap (`scripts/gen_unicode_tables.py`
      → `src/utf_tables.c`, ~12 KB total, UCD 15.0.0, 8612 punct
      codepoints). `mdit_is_whitespace` is hand-rolled to match
      markdown-it's explicit set (Zs subset + ASCII whitespace).
      4 tests, including a 8551-codepoint sample cross-checked against
      Python's `unicodedata`.
- [x] **`entities.{h,c}`** — HTML5 entity lookup. Generated from
      `html.entities.html5` (`scripts/gen_entities.py` →
      `src/entities_table.c`, 2125 entries, ~36 KB). Sorted name pool
      with binary-search lookup, case-sensitive. 8 tests including a
      cross-check vector header populated from real Python.
- [x] **`json.{h,c}`** — minimal JSON encoder matching Python's
      `json.dumps(..., ensure_ascii=False)` for null/bool/int/double/
      string. 7 tests covering escapes, control-char passthrough, and
      UTF-8 verbatim emission.
- [x] **`token.{h,c}`** — struct + setters + `attr_set/get/join`,
      arena-managed children vector, and `mdit_token_to_json` whose
      output byte-matches `json.dumps(Token.as_dict(as_upstream=True),
      ensure_ascii=False)`. 12 tests including 9 generated vectors
      built from real `markdown_it.token.Token` shapes (paragraph,
      link with attrs+map, code_inline, empty/non-empty children,
      UTF-8 content, hidden flag, hr).
- [x] **`ruler.{h,c}`** — ordered registry mirroring `markdown_it.ruler`.
      `push/before/after/at/enable/disable/enable_only/get_rules` plus
      alt-chain caching, dirty-bit invalidation, status-coded errors.
      9 tests covering ordering, alt chains, duplicate detection,
      and introspection.
- [x] **`url.{h,c}`** — full `mdurl` 0.1.x port (`parse`, `format`,
      `encode`, `decode`). Hand-written scanners replace the four
      mdurl regex patterns; encode/decode skip the per-exclude cache
      dictionary in favour of inline 128-byte boolean lookup tables.
      8 tests including 28 parse cases (auth/port/IPv6/hostless
      protocols/unicode hostname-validation fallback) plus encode +
      decode vectors covering Latin-1, CJK, supplementary plane, and
      malformed UTF-8 — every byte cross-checked against Python mdurl.
- [x] **`escape.{h,c}`** — `escapeHtml` (replaces & < > " only — single
      quotes are NOT escaped, matching upstream), `unescapeAll`
      (CommonMark backslash escapes + named/numeric HTML entities,
      using the entity table; invalid entity references pass through
      verbatim), `isValidEntityCode`, and `mdit_emit_utf8`. 8 tests.
- [x] **`renderer.{h,c}`** — token-stream → HTML driver. Registry of
      per-token-type render functions plus default rules for
      `code_inline`, `code_block`, `fence` (with `langPrefix`),
      `image` (with `renderInlineAsText` for the alt attribute),
      `hardbreak`, `softbreak` (with `breaks` / `xhtmlOut`),
      `text`, `html_block`, `html_inline`. The default `renderToken`
      mirrors upstream's tight-list / "newline before next block"
      logic. `mdit_renderer_add_rule` lets plugins override or extend.
      14 tests including custom-rule replacement, attr escaping, and
      every default rule.
- [ ] `common/normalize_url` (`normalizeLink` / `normalizeLinkText` /
      `validateLink`) — defers to Phase 2 since it composes url +
      escape and is consumed exclusively by `MarkdownIt.normalizeLink`.

**Exit criterion (MET):** all 12 ctest suites pass under MSVC `/W4 /WX`;
the JSON dump of hand-crafted token streams byte-matches Python (9
shapes in `tests/token_vectors.h`); URL parse / format / encode /
decode byte-match `mdurl` for 28 + ~20 + ~10 cases; every Phase 1
foundation has cross-checked unit-test coverage.

### Phase 2 — core + block parser *(in progress)*

Implement enough to run blocks against the oracle.

- `state_core`, `state_block`, `parser_core`, `parser_block`.
- Block rules in dependency order: `code` → `fence` → `heading` → `hr`
  → `lheading` → `paragraph` → `blockquote` → `list` → `reference`
  → `html_block` → `table`.
- Core rules: `normalize`, `block`.

**Exit criterion:** all block-only fixtures pass; `mdit_parse` token
streams match the oracle byte-for-byte.

**Slice 2a — scaffolding *(done)*:**

| Module                       | Status |
| ---------------------------- | :----: |
| `state.{h,c}` (core/block/inline state objects) | ✅ |
| `parser_core.{h,c}` (ruler + driver)            | ✅ |
| `parser_block.{h,c}` (ruler + tokenize loop)    | ✅ |
| `parser_inline.{h,c}` (ruler + ruler2 + tokenize) | ✅ |
| `main.{h,c}` (`MarkdownIt` facade, parse/render) | ✅ |
| Core rule: `normalize`                          | ✅ |
| Core rules: `block`, `inline` (drivers)         | ✅ |
| Block rule: `paragraph`                         | ✅ |
| Inline rule: `text`                             | ✅ |
| End-to-end smoke tests (`test_main.c`)          | ✅ |

`hello`, `foo\n\nbar`, `a<b&c>d`, CRLF/CR/NUL normalization all
produce identical HTML to upstream. The 9 `test_main` cases plus all
12 Phase 1 suites pass under MSVC `/W4 /WX` Debug.

**Slice 2b — block rules half *(done)*:**

| Rule          | Status |
| ------------- | :----: |
| `code` (indented 4-space)             | ✅ |
| `fence` (``` / ~~~ + lang/info)       | ✅ |
| `blockquote` (incl. nested + lazy continuation) | ✅ |
| `hr` (`---` / `***` / `___`)          | ✅ |
| `heading` (ATX `#`..`######`)         | ✅ |
| `lheading` (Setext `=`/`-` underline) | ✅ |

All registered with the right `alt`-chain tags so `paragraph` /
`blockquote` / `list` rule chains fire correctly. State helpers added:
`skip_chars`, `skip_chars_back`, `skip_spaces_back`, `is_code_block`.

A new `tests/block_oracle.h` is auto-generated by
`scripts/gen_block_oracle.py` from `MarkdownIt('default').render(...)`
and pinned in CI; `test_block_rules.c` runs all 51 cases against the C
parser and asserts byte-identical HTML output. Cases cover paragraphs,
all six ATX heading levels, Setext headings, three HR markers, indented
+ fenced code (with lang info, unclosed fences, mixed-indent bodies),
blockquotes (nested up to 3 deep, lazy continuation, blank-inside,
followed-by-para/hr), and rule-ordering combinations.

**Slice 2c — `list` + inline `newline` *(done)*:**

| Rule              | Status |
| ----------------- | :----: |
| `list` (bullet `*`/`-`/`+` + ordered `\d+[.)]`) | ✅ |
| `list` recursion (item content via `tokenize`)  | ✅ |
| `list` tight/loose detection + `markTightParagraphs` | ✅ |
| `list` ordered-`start` attribute                | ✅ |
| `list` "must start at 1" paragraph-termination quirk | ✅ |
| Inline rule: `newline` (soft + hard breaks, leading-ws skip) | ✅ |

Two oracle generators:

- `scripts/gen_block_oracle.py` — 202 hand-curated cases covering
  every shipped rule + edge case (heading levels, fence info strings,
  blockquote up to 3 levels deep, lazy continuation, list tightness
  on/off, ordered/bullet styles, escape/backtick/entity inline mixes,
  reference definitions, html_block / html_inline (`html=true`),
  GFM tables (alignments, autocomplete, escape pipes, termination,
  invalid shapes), emphasis / strong delimiter balancing and
  fragments-join cleanup, mixed scenarios). Each row carries a
  per-case `opts` bitmap so cases that need non-default options ride
  the same harness.
- `scripts/sweep_block_oracle.py` — sweeps `tests/test_cmark_spec/
  commonmark.json`, filters to cases that don't fire any rule the C
  port hasn't ported yet, and appends them after the hand cases. 338
  spec cases land today; that grows automatically as more inline
  rules ship — the filter is loosened in lockstep.

The combined oracle (currently 540 cases) is pinned in CI: the sweep
script is the regen path, and the workflow fails on any drift between
the committed `block_oracle.h` and a fresh regen.

**Slice 2d — block rules:** complete.
`reference`, `html_block`, and `table` (GFM) shipped, alongside
inline `html_inline` so HTML embedded in paragraphs round-trips when
`html=true`. `reference` consumes definitions (with multiline labels
/ titles), validates normalized destinations, and records first-wins
+ duplicates in a parse env. `html_block` ports the seven CommonMark
sequences (raw-text tags, comments, processing instructions,
declarations, CDATA, block-name tags, and arbitrary open/close tags
ending the line). `table` does GFM cell splitting (with `\|` escapes),
divider validation against `^:?-+:?$`, alignment styles, autocomplete
of short rows, and termination via the `blockquote` alt chain.

**Phase 2 status:** all eleven block rules and the four core rules
(`normalize`, `block`, `inline`, `text_join`) are now ported. The
remaining work is in Phase 3 (inline rules) and Phase 4 (renderer +
remaining core rules).

### Phase 3 — inline parser

- `state_inline`, `parser_inline`.
- Inline rules: `text` → `newline` → `escape` → `backticks` → `entity`
  → `autolink` → `html_inline` → `link` → `image` → `emphasis`
  → `balance_pairs` → `fragments_join` → `strikethrough` → `linkify`.
- Helpers: `parse_link_destination`, `parse_link_label`, `parse_link_title`.
  (`parse_link_destination` and `parse_link_title` are already shipped
  with the `reference` block rule.)

**Slice 3d — tasklists + alerts (`mdit-py-plugins` corpus)** ✅ shipped:
GFM tasklists and GitHub-style alerts, both gated behind opt-in
options (`tasklists` / `tasklists_editable`, `alerts`) so default-preset
behaviour is unchanged. Tasklists piggy-back on the existing `list`
block rule: a 4-byte `[ ] `/`[x] `/`[X] ` peek at item-content start
sets `meta["checked"]` on the `list_item_open` token, advances
`bMarks[startLine]` past the checkbox, and a post-list pass adds
`task-list-item` / `contains-task-list` classes (mirroring upstream
exactly). The renderer overrides `list_item_open` to emit
`<input class="task-list-item-checkbox" disabled="" type="checkbox" …>`
(omitting `disabled` when `tasklists_editable=True`). Alerts extend
the `blockquote` rule: after the existing line-scan loop a `[!KIND]`
detector inspects the first content line, and on a match emits
`alert_open` (`div.markdown-alert.markdown-alert-<kind>`) +
`alert_title_open`/inline title (kind-capitalised) + body-tokenize
(skipping the marker line) + `alert_close`. Block oracle gained
15 + 14 hand-curated cases; token oracle picked up two new sources
(`tasklists`, `alerts`).

**Slice 3c — strikethrough** ✅ shipped:
GFM `~~text~~` (and the optional `~text~` / `gfm-like2`-mode opt-in via
`options.strikethrough_single_tilde`). Tokenizer pushes `~` runs onto
the delimiter list with `length=0` so emphasis's "rule of three"
length checks don't apply; the actual marker width travels in the text
token's content and is consulted during the post-process to gate
single-tilde matches. Post-process rewrites paired delimiters to
`s_open`/`s_close` and shuffles stray `~` markers past subsequent
`s_close` runs (mirrors upstream's `loneMarkers` handling). Registered
between `backticks` and `emphasis` in the inline ruler, and between
`balance_pairs` and `emphasis` in ruler2 — same order as upstream.
Disabled in the `commonmark` preset so `MarkdownIt('commonmark')` /
the C CLI keep CommonMark-strict behaviour. The block oracle gained
15 hand-curated strikethrough cases; the token oracle gained 15 +
13 cases (`strikethrough` + `strikethrough_single_tilde` sources).

**Slice 3a — small inline rules** ✅ shipped:
`text` and `newline` came in with Phase 2's scaffolding. Slice 3a
added `escape` (`\X` punctuation + hard-break), `backticks` (`` ` ``
code spans with marker-length matching, leading/trailing space
stripping, newline normalization), and `entity` (`&name;`, `&#N;`,
`&#xN;`). Also ported the `text_join` core rule that converts
`text_special` → `text` and merges adjacent text tokens. Sweep filter
loosened by `\`, `` ` ``, `&`; corpus grew from 163 → 188 spec cases.

**Slice 3b — emphasis pairs** ✅ shipped:
Added the delimiter vector to `StateInline`, ported `emphasis`
tokenization (`*` / `_` delimiter runs), `balance_pairs` (linear
delimiter matching with openers-bottom and rule-of-three checks),
emphasis post-processing (`em` / `strong` token rewriting), and
`fragments_join` (level recalculation + adjacent text compaction).
Sweep filter loosened by `*` and `_`; corpus grew from 188 → 338 spec
cases.

**Exit criterion:** full CommonMark spec passes via `mdit_render`.

### Phase 4 — remaining core rules + renderer parity

- Remaining core rules: `inline`, `linkify`, `replacements`,
  `smartquotes`, `text_join`.
- Full `RendererHTML` parity: XHTML mode, `breaks`, `langPrefix`,
  custom render rules.
- Optional `SyntaxTreeNode` convenience layer.

**Exit criterion:** all CommonMark spec tests + all `tests/test_port`
cases produce identical HTML to Python.

### Phase 5 — bindings, CLI, packaging *(in progress)*

- [x] **`cli/md_cli`** — `markdown-it-c` command-line driver. Mirrors
      `markdown_it/cli/parse.py`'s batch + `--stdin` modes (interactive
      REPL is intentionally omitted — pipelines are the natural fit).
      Loads the `commonmark` preset to byte-match `python -m
      markdown_it.cli.parse`: `xhtmlOut=True`, `html=True`,
      `maxNesting=20`, GFM `table` rule disabled. Argv: `-h/--help`,
      `-v/--version`, `--stdin`, `--`, then one or more filenames.
      Exit codes: `0` ok, `1` file open/read error, `2` render error,
      `64` usage error (sysexits-style). Smoke + parity tests live in
      `cli/`: `md_cli_version`, `md_cli_help`, `md_cli_unknown_option`,
      and `md_cli_python_parity` (a Python-driven byte-equality sweep
      against the upstream CLI; auto-skips if `markdown_it` isn't
      importable in the test interpreter).
- [~] **CPython extension** (`mdit-c/bindings/python/`) — slices 1–2
      shipped. `MDIT_BUILD_PYBIND=ON` builds an internal `_mdit_c`
      module via CMake's `Python3_add_library(... WITH_SOABI ...)` and
      stages it next to a small pure-Python `mdit_c` package wrapper.
      The exposed surface today is `MarkdownIt(preset='default',
      options=None)`, `.render(src) -> str`, `.parse(src, env=None) ->
      list[Token]`, the read/write `.options` dict, and `.enable` /
      `.disable` against any of the four rulers (core / block / inline
      / inline ruler2). Presets mirror the upstream Python ones
      (`default`, `commonmark`, `zero`); options accept the same
      camelCase keys upstream uses (`maxNesting`, `xhtmlOut`,
      `langPrefix`, …). Slice 2 added a copied Python `Token` type with
      public fields matching `markdown_it.token.Token`, a constructor
      accepting the same core fields, equality, `copy(**changes)`,
      `from_dict(...)`, `as_dict(...)` support (including upstream
      attrs-as-list/null and recursive child conversion), and the common
      attr helpers (`attrIndex`, `attrItems`, `attrGet`, `attrSet`,
      `attrPush`, `attrJoin`).
      `python_smoke` now covers 7 hand-curated `commonmark` render
      cases, 2 `default`-preset render cases, GFM tasklists + alerts
      behaviour, token field / attr-helper sanity checks, and a
      14-input byte-parity sweep against both
      `markdown_it.MarkdownIt('commonmark').render(...)` and
      `[t.as_dict(as_upstream=True) for t in ...parse(...)]`.
      The `_DEBUG` swap around `<Python.h>` lets the extension build
      under MSVC's Debug config without `python3XX_d.lib`. CI gained
      the `MDIT_BUILD_PYBIND=ON` flag in the matrix + sanitizer jobs.
      Full upstream `pytest` hookup, renderer/ruler Python facades, and
      `SyntaxTreeNode` remain follow-up slices.
- [x] **CMake install rules + package config + pkg-config**. Toggled by
      `MDIT_INSTALL` (default ON). Installs `mdit_static` (renamed
      `libmdit.{a,lib}`) under `CMAKE_INSTALL_LIBDIR`, the curated
      `include/mdit/mdit.h` header plus the current `src/*.h` impl
      headers under `CMAKE_INSTALL_INCLUDEDIR/mdit/`, and the
      `markdown-it-c` CLI under `CMAKE_INSTALL_BINDIR`. Generates a
      relocatable `mditConfig.cmake` (downstream calls
      `find_package(mdit REQUIRED)` → `mdit::mdit`), a
      `mditConfigVersion.cmake` matching the project version with
      `SameMinorVersion` compatibility, and a `mdit.pc` for pkg-config
      consumers. The PUBLIC include surface is wrapped in
      `BUILD_INTERFACE`/`INSTALL_INTERFACE` generator expressions so a
      single export works from both build and install trees. A new
      `install_smoke` CTest exercises the whole chain end-to-end:
      stage-install → configure a tiny consumer with
      `find_package(mdit)` → build → run → assert HTML output for
      `# hi\n`. Versioned shared-lib build is left for a follow-up
      slice (it requires deciding on symbol-visibility export macros
      across all `src/*.h` headers).

### Phase 6 — hardening

- libFuzzer / AFL++ harnesses mirroring `tests/fuzz/`.
- ASan / UBSan / MSan jobs in CI.
- Benchmarks vs Python and vs `cmark`.
- Doxygen API docs and a porting guide for plugin authors.

## 3. Risk register

| Risk                                                    | Mitigation                                                              |
| ------------------------------------------------------- | ----------------------------------------------------------------------- |
| Regex semantics drift                                   | Hand scanners + UCD-generated tables; oracle byte-compare per case      |
| Plugin ecosystem (`mdit-py-plugins`, `myst-parser`)     | Out of scope; CPython extension keeps the Python API alive              |
| Off-by-one between Python str indexing and UTF-8 bytes  | Strict invariant: parser positions are codepoint indices; asserts + ASan |
| Memory bugs in C                                        | Arena + ASan/UBSan from day one + libFuzzer corpus                      |
| `mdurl` replacement non-trivial                         | Port as a self-contained module with its own ported test suite          |
| Unicode/entity tables go stale                          | Regenerate via `scripts/`; CI runs the generators to detect drift       |

## 4. Effort estimate (single experienced C dev)

| Phase | Estimate     |
| ----- | ------------ |
| 0     | 0.5–1 week   |
| 1     | 2–3 weeks    |
| 2     | 3–4 weeks    |
| 3     | 3–4 weeks    |
| 4     | 1–2 weeks    |
| 5     | 1–2 weeks    |
| 6     | ~2 weeks (then ongoing) |

Total to a 1.0-equivalent: **~3–4 months**.
