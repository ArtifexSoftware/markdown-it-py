# Porting markdown-it-py plugins to mdit-c

This guide walks plugin authors from the upstream Python plugin
contract (as documented in `markdown-it-py`'s `docs/architecture.md`
and the `mdit-py-plugins` repository) to the equivalent C / CPython
shapes. Three layers are covered:

1. **C plugins** — calling the C engine directly with no Python in
   the loop. Smallest binary footprint, highest throughput.
2. **CPython plugins** — using the bundled `mdit_c` Python package,
   which re-emits upstream `markdown_it.Token` instances and accepts
   ordinary Python rule callables.
3. **Renderer overrides** — the same shape in both worlds.

Throughout we use a running example: a fictional **`shrug`** plugin
that turns a literal `:shrug:` token into the text `¯\_(ツ)_/¯`.

## Concepts that didn't change

The runtime shape is identical to upstream:

* Three rule chains: **core**, **block**, **inline**, plus the
  inline `ruler2` chain for post-processing. A plugin installs zero
  or more rules into each.
* Each rule is a function. Block and inline rules return `bool` /
  `int` indicating whether they consumed input; core rules return
  `void` / `None`.
* Tokens are emitted into a flat token stream. Inline tokens become
  the `children` of their wrapping `inline` token after the inline
  parser runs.
* The renderer maps token `type` to a render function. Plugins
  override / register render functions through the same registry.

If you've written a `markdown-it-py` plugin, there's nothing new to
learn structurally — only the names and the storage discipline
change.

## Layer 1 — pure-C plugins

A C plugin is a function that receives the live `mdit_md` and
installs rules through its parser-specific rulers.

```c
#include <mdit/main.h>
#include <mdit/parser_inline.h>
#include <mdit/state.h>
#include <mdit/token.h>

static bool rule_shrug(mdit_state_inline *st, bool silent)
{
    static const char NEEDLE[] = ":shrug:";
    const size_t      N        = sizeof(NEEDLE) - 1;

    if (st->pos + N > st->pos_max) return false;
    if (memcmp(st->src.data + st->pos, NEEDLE, N) != 0) return false;

    if (!silent) {
        mdit_token *t = mdit_state_inline_push(
            st, MDIT_STR_LIT("text"), MDIT_STR_LIT(""), 0);
        t->content = MDIT_STR_LIT("¯\\_(ツ)_/¯");
    }
    st->pos += N;
    return true;
}

void shrug_plugin(mdit_md *md)
{
    /* Insert before the default `text` rule so we win the prefix
     * match before `text` accepts the leading colon. */
    mdit_ruler_inline_before(
        &md->parser_inline.ruler,
        MDIT_STR_LIT("text"),
        MDIT_STR_LIT("shrug"),
        rule_shrug,
        (mdit_rule_options){0});
}
```

A few invariants every C plugin must respect:

* **Allocation lives in the engine arena.** Strings stored on a
  token (`content`, `info`, `markup`, attribute keys/values…) must
  point at memory that outlives the parse. Use
  `mdit_arena_dup_str(md->arena, ...)` to copy short strings, or
  embed string literals directly (`MDIT_STR_LIT("...")`) — those
  have static storage.
* **`silent`-mode rules must not push tokens.** They exist so the
  parser can probe ahead during emphasis matching. Returning `true`
  in silent mode just claims "yes, I would have parsed this".
* **`add_terminator_char` for non-default triggers.** If the rule's
  trigger character is anything other than the defaults, register
  it through `mdit_parser_inline_add_terminator(&md->parser_inline,
  ch)` so the inline `text` rule knows to stop at it.
* **Token-stream mutations during a core rule** go straight into
  `state->tokens`. Use `mdit_vec_token_push(state->tokens, t)` or
  splice in place; the engine doesn't checkpoint.

See `mdit-c/src/rules_inline/strikethrough.c` and
`mdit-c/src/rules_block/list.c` for production examples.

## Layer 2 — CPython plugins (recommended for porting from Python)

The `mdit_c` Python package re-exports an upstream-shaped surface
on top of the C engine. Almost every existing `markdown-it-py`
plugin runs unmodified after a one-line import change:

```python
# from markdown_it import MarkdownIt
from mdit_c import MarkdownIt

def shrug_plugin(md):
    def rule_shrug(state, silent):
        if not state.src.startswith(":shrug:", state.pos):
            return False
        if not silent:
            tok = state.push("text", "", 0)
            tok.content = "¯\_(ツ)_/¯"
        state.pos += len(":shrug:")
        return True

    md.inline.ruler.before("text", "shrug", rule_shrug)

md = MarkdownIt().use(shrug_plugin)
print(md.render("hello :shrug: world"))
```

What's the same:

* `md.use(plugin, *args, **kwargs)` for registration.
* `md.add_render_rule("type", fn)` for renderer overrides.
* `md.{core,block,inline}.ruler.{before,after,at,push}(...)` for
  rule installation. The `options={"alt": [...]}` keyword is
  honoured.
* `state` exposes `src`, `pos`, `pos_max` (`posMax` for inline),
  `level`, `env`, `md`, plus chain-specific scalars (`startLine`,
  `endLine`, `tight`, `parentType` for block; `pos`/`pending` for
  inline; `inlineMode` for core).
* `state.tokens` is a live `list[Token]`. Append, replace, or
  reassign — all three are written back into the C engine when the
  rule returns.
* `state.push(type, tag, nesting)` returns a freshly-attached
  `Token` and bumps `state.level` automatically (matches upstream).
* Tokens are real `markdown_it.Token`-shaped objects (same fields,
  same `as_dict` / `from_dict`, same `attr*` helpers).

What's different:

* Inline-rule trigger characters that aren't already in the default
  set must be announced:
  `md.inline.add_terminator_char(":")`. (The upstream API is named
  the same; the difference is that mdit-c never falls back to the
  Python-level "scan every position" path, so missed terminators
  silently break the rule rather than just slowing it down.)

## Layer 3 — renderer overrides (both worlds)

Identical to upstream:

```python
def render_shrug(self, tokens, idx, options, env):
    return '<span class="shrug">¯\\_(ツ)_/¯</span>'

md.add_render_rule("shrug", render_shrug)
```

`self` is the renderer facade and exposes `renderToken`,
`renderInline`, `renderInlineAsText`, `renderAttrs` for delegation.
The C-side equivalent lives in `mdit_renderer_set_rule(...)`; see
`mdit-c/src/renderer.h`.

## Porting checklist

1. **Identify the rule chain(s)** the upstream plugin touches.
   `markdown_it.MarkdownIt.disable("...")` calls and
   `md.{core,block,inline}.ruler.before(...)` lines tell you which.
2. **Copy the rule body verbatim** at the CPython layer first. If
   it works there, you've already proved correctness against the
   upstream test corpus.
3. **Profile.** If the plugin is hot enough that the Python
   trampoline cost matters, drop the rule down to layer 1 (C). The
   token-stream contract is byte-identical between layers, so the
   downstream renderer doesn't need to change.
4. **Add a regression test.** The
   `mdit-c/bindings/python/upstream_tests/` directory mirrors the
   upstream `tests/test_port` structure. Drop a `test_<plugin>.py`
   file in alongside; it'll get exercised by the existing
   `python_pytest` CTest entry.
5. **(Optional) Ship a wheel.** The CPython extension is a single
   compiled module; a `pyproject.toml` that depends on `mdit_c`
   will pip-install on Linux/macOS/Windows.

## Limitations

The C engine doesn't currently expose:

* Custom attribute serialisers per renderer (the HTML attribute
  format is hard-coded; matches upstream).
* Rule-level enable/disable through `options={"alt": [...]}` for
  parser chains other than the four shipped ones (core, block,
  inline, inline2). This matches upstream semantics.
* Streaming / incremental parse — `mdit_md_parse` always consumes
  its full input. Same as upstream.

If you need something that isn't covered above, file an issue; most
gaps are one targeted CPython binding away from working.
