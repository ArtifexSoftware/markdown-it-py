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
- [~] **CPython extension** (`mdit-c/bindings/python/`) — slices 1–6
      shipped. `MDIT_BUILD_PYBIND=ON` builds an internal `_mdit_c`
      module via CMake's `Python3_add_library(... WITH_SOABI ...)` and
      stages it next to a small pure-Python `mdit_c` package wrapper.
      The exposed surface today is `MarkdownIt(config='commonmark',
      options=None)` (with the `preset=` / `options_update=` keyword
      aliases upstream uses), `.render(src) -> str`,
      `.parse(src, env=None) -> list[Token]`, the live `.options`
      mapping, and `.enable` / `.disable` against any of the four
      rulers (core / block / inline / inline ruler2). Slice 3 made
      `.options` a *persistent* dict that's resynced into the C
      `mdit_options` on every parse/render — so upstream's
      `md.options['typographer'] = True` mutation pattern works — and
      grew the preset table to cover `default` / `js-default`,
      `commonmark` (the new constructor default, matching upstream),
      `zero`, `gfm-like`, and `gfm-like2`. Options accept the same
      camelCase keys upstream uses (`maxNesting`, `xhtmlOut`,
      `langPrefix`, `quotes` as a 4-codepoint string or 4-element
      sequence, `linkify`, `typographer`, `tasklists`, `alerts`,
      `strikethrough_single_tilde`, `tasklists_editable`, …). Slice 2
      had already added a copied Python `Token` type with public
      fields matching `markdown_it.token.Token`, a constructor
      accepting the same core fields, equality, `copy(**changes)`,
      `from_dict(...)`, `as_dict(...)` support (including upstream
      attrs-as-list/null and recursive child conversion), and the
      common attr helpers (`attrIndex`, `attrItems`, `attrGet`,
      `attrSet`, `attrPush`, `attrJoin`).
      Test coverage:
      * `python_smoke` covers 7 hand-curated `commonmark` render
        cases, 2 `default`-preset render cases, GFM tasklists +
        alerts behaviour, the `gfm-like` / `gfm-like2` preset
        wirings, mutating `md.options['typographer']` post-init,
        token field / attr-helper sanity checks, and a 14-input
        byte-parity sweep against both
        `markdown_it.MarkdownIt('commonmark').render(...)` and
        `[t.as_dict(as_upstream=True) for t in ...parse(...)]`.
      * **`python_pytest`** (slice 3) drives the upstream Python test
        corpus directly via `pytest`. `mdit-c/bindings/python/upstream_tests/`
        ships a conftest + four parametrised modules: full CommonMark
        spec corpus (~650 cases through `tests/test_cmark_spec/commonmark.json`),
        `test_no_end_newline.py`, a `test_misc.py::test_ordered_list_info`
        port, and a 13-fixture `test_fixtures.py` that mirrors
        upstream `tests/test_port/test_fixtures.py`
        (`linkify`, `smartquotes`, `typographer`, `tables`,
        `commonmark_extras`, `normalize`, `fatal`, `strikethrough`,
        `strikethrough_single_tilde`, `disable_code_block`,
        `tasklists`, `alerts`, `issue-fixes`). On Windows + Python 3.12
        all 904 collected items pass byte-parity. The CTest entry is
        driven by `run_upstream_pytest.py`, which exits 0 with a SKIP
        marker when `pytest` or `markdown_it` isn't importable so
        dependency-light hosts still pass.
      Slice 3 also surfaced and fixed a behavioural divergence in the
      C parser: when the `code` block rule is disabled, indented
      content now falls through to the remaining rules (fence,
      heading, table, …) instead of being routed into the indented
      code-block path — matching upstream. The block parser threads
      a `code_enabled` flag (sourced from the new
      `mdit_ruler_is_rule_enabled` helper) into
      `mdit_state_block_is_code_block` for that purpose.
      The `_DEBUG` swap around `<Python.h>` lets the extension build
      under MSVC's Debug config without `python3XX_d.lib`. CI gained
      the `MDIT_BUILD_PYBIND=ON` flag in the matrix + sanitizer jobs.
      Slice 4 layered Python-side `Ruler` and `RendererHTML` facades
      on top of the C extension: `md.{core,block,inline}.ruler`
      expose `get_all_rules`, `get_active_rules`, `enable`, `disable`,
      `enableOnly`; `md.add_render_rule(name, fn)` registers a Python
      callback that the C renderer invokes through a bridge (passing
      `Token` copies, the live options dict, and the `env` argument
      from `render(src, env=...)`); `md.use(plugin, ...)` and
      `md.reset_rules()` mirror the upstream chainable API.
      Slice 5 added inline-only parse/render entry points and
      env propagation: `mdit_md_parse_inline` /
      `mdit_md_render_inline` flip `state.inlineMode`; the CPython
      bindings expose them as `parseInline(src, env=None)` and
      `renderInline(src, env=None)`. Both `parse`/`render` and their
      inline counterparts now copy the C parser's
      `mdit_env.references` (and `duplicate_refs`) back into the
      user-supplied Python `env` mapping using upstream's exact
      shape (`env["references"][LABEL] = {"title", "href", "map"}`,
      `env["duplicate_refs"]` for collisions), and reject non-
      `MutableMapping` env arguments with a `TypeError` matching
      upstream's runtime check. New tests:
      `upstream_tests/test_inline_and_env.py` covers token shape
      from `parseInline`, `renderInline` skipping `<p>` wrap,
      `env["references"]` / `env["duplicate_refs"]` parity vs
      `markdown_it`, and TypeError on non-mapping env.
      Slice 6 ports `markdown_it.tree.SyntaxTreeNode` to
      `mdit_c.tree.SyntaxTreeNode` (also re-exported as
      `mdit_c.SyntaxTreeNode`). The class consumes the existing
      `_mdit_c.Token` public surface, so the port is a near-verbatim
      copy of the upstream module: ``children`` / ``parent`` /
      ``walk`` / ``to_tokens`` / ``pretty`` / ``next_sibling`` /
      ``previous_sibling`` / property pass-through (``tag``,
      ``attrs``, ``map``, ``level``, ``content``, ``markup``,
      ``info``, ``meta``, ``block``, ``hidden``).
      `upstream_tests/test_tree.py` mirrors the upstream
      `tests/test_tree.py` cases (token round-trip, type, sibling
      traversal, walk order, `pretty(show_text=True)` byte-equal to
      the upstream `.xml` regression files, plus a top-level
      `mdit_c.SyntaxTreeNode` re-export check).
      Slice 7 wires Python parser rule callbacks through
      `ruler.before/after/at/push` for the `core`, `block`, `inline`,
      and `inline2` chains. `mdit_ruler` now distinguishes built-in
      typed C rules from generic callback rules and dispatches plugin
      entries with their `user` pointer. The CPython binding roots each
      callback on the `MarkdownIt` instance, installs the right bridge
      for the chain, and exposes short-lived state wrappers:
      `StateCore(src, md, env, inlineMode)`,
      `StateBlock(src, md, env, line/lineMax/blkIndent/level/tight/
      parentType/...)`, and `StateInline(src, md, env, pos/posMax/
      level/pending/pendingLevel/linkLevel)`, with `StateInline.pos`
      writable so simple consuming rules can advance the cursor.
      `upstream_tests/test_plugin_creation.py` mirrors the upstream
      callback smoke cases (core/block/inline before/after/at through
      `md.use`) and adds checks for inline2, state invalidation after a
      rule returns, enable/disable on plugin rules, and registration
      error paths.
      Slice 8 closes out the plugin-surface gaps: (1) every state
      wrapper exposes `state.tokens` as a live list of `Token`
      instances — reads materialise lazily from the C
      `mdit_vec_token` (or the inline parent's children array) and
      mutations (`append`, in-place edits, `state.tokens = [...]`)
      are folded back into the C engine when the rule returns, with
      strings/attrs/meta/children duplicated into the parser's
      arena; (2) the user-supplied `env` argument from
      `parse`/`render` is forwarded as `state.env` on every callback
      (it threads through a new `mdit_env.user` slot the C engine
      ignores); (3) `ruler.before/after/at/push` accepts
      `options={"alt": [...]}` to register rules under alt-chain
      tags (already supported by the C `mdit_ruler`, now plumbed
      through `_ruler_install`); (4) `MarkdownIt.inline.
      add_terminator_char(ch)` registers a single ASCII character
      that stops the inline `text` rule, mirroring upstream's
      `ParserInline.add_terminator_char`. New `test_plugin_creation`
      cases exercise env identity, `state.tokens` round-trips for
      core/block/inline rules, alt-chain options, and the upstream
      `test_add_terminator_char` scenario byte-for-byte.
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
      `# hi\n`.
- [x] **Versioned shared library**. Adds `MDIT_BUILD_SHARED` (default
      OFF). When ON the `mdit_static` target is built as a SHARED
      library with `VERSION=${PROJECT_VERSION}` and `SOVERSION=
      ${PROJECT_VERSION_MAJOR}` (POSIX gets the standard
      `libmdit.so.0.0.1` + symlink chain; Windows gets `libmdit.dll`
      + `libmdit.dll.a` import lib). On MSVC `WINDOWS_EXPORT_ALL_SYMBOLS`
      auto-emits the public surface so we don't have to thread
      `__declspec(dllexport)` through internal headers in this slice;
      `MDIT_SHARED` / `MDIT_BUILDING` are defined to leave a clean
      seam for tightening visibility later. CMake build-tree
      `RUNTIME_OUTPUT_DIRECTORY` is unified to `bin/` on Windows so
      every executable finds the DLL without PATH gymnastics, and
      `install_smoke/run_smoke.cmake` copies the staged DLL beside
      the smoke binary on Windows / sets `LD_LIBRARY_PATH` /
      `DYLD_LIBRARY_PATH` on POSIX so the install-consumer test
      exercises the dynamic-link path end-to-end. While there, fixed
      a latent CMake bug where `${CMAKE_INSTALL_INCLUDEDIR}` resolved
      to empty inside the library's `INSTALL_INTERFACE` includes
      (`include(GNUInstallDirs)` was being called *after* the target).

### Phase 6 — hardening

- libFuzzer / AFL++ harnesses mirroring `tests/fuzz/` — DONE.
    - `mdit-c/fuzz/` ships three harnesses:
      `fuzz_parse_render` (full pipeline, mirrors upstream
      `fuzz_markdown.py`), `fuzz_inline` (`renderInline` slice), and
      `fuzz_token_roundtrip` (parse + `mdit_tokens_to_json`, the
      contract used by the token oracle).
    - Each harness defines `LLVMFuzzerTestOneInput` and links one of
      two drivers depending on toolchain availability:
      coverage-guided fuzzing under
      `-DMDIT_FUZZ_LIBFUZZER=ON` (Clang only; pulls in
      `-fsanitize=fuzzer,address,undefined`), or a portable
      replay driver (`fuzz_main.c`) that walks a corpus directory
      and replays each file through `LLVMFuzzerTestOneInput`. The
      latter mode works on every supported compiler.
    - `corpus/<harness>/` holds shrunk inputs covering the upstream
      regression shapes (headings, lists, tables, references, raw
      HTML, fenced code, escapes, emoji). Failing inputs from
      oss-fuzz get committed here so they stay green forever.
    - `MDIT_BUILD_FUZZ=ON` opt-in; the in-tree CTest sweep gains
      three `fuzz_smoke_*` entries that replay the corpus and exit
      well under a second per harness.
- ASan / UBSan / MSan jobs in CI — DONE.
    - `.github/workflows/mdit-c.yml`: the existing `sanitizers`
      job (Linux + Clang, `-DMDIT_ENABLE_ASAN=ON
      -DMDIT_ENABLE_UBSAN=ON`) now also enables `MDIT_BUILD_FUZZ=ON`
      so the fuzz smoke entries run under sanitizers.
    - New `c-fuzzers` job: builds with
      `-DMDIT_FUZZ_LIBFUZZER=ON` (clang + libFuzzer) and runs each
      harness for 30 seconds against the seeded corpus, uploading
      crash artifacts on failure. Real fuzzing remains on oss-fuzz;
      this CI job exists to keep the harnesses building cleanly and
      to catch regressions on the seed corpus immediately.
    - `cmake-configure` matrix gained `-DMDIT_BUILD_FUZZ=ON` so the
      smoke entries run on every supported platform/compiler.
    - MSan deferred: requires a Clang-built libc++ to avoid false
      positives on the C++ `cstdlib` shim used in tests; lower
      priority than the ASan + UBSan + libFuzzer trio that's now in.
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
