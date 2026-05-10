# mdit-c — C11 port of markdown-it-py

`mdit-c` is a CommonMark / GFM Markdown engine written in portable
C11. The API is a thin, allocator-aware translation of the Python
`markdown_it` package, designed so that the existing plugin
ecosystem ports cleanly to C while still being usable from Python
through the shipped CPython extension.

## Library layout

| Header | Purpose |
| ------ | ------- |
| `mdit/mdit.h`          | curated public API (versioning, status codes, `mdit_ctx`) |
| `mdit/mdit_lib_ctx.h` | allocator hooks + OOM callback (`mdit_lib_ctx`); passed explicitly to arena and `mdit_md` APIs |
| `mdit/main.h`     | `mdit_md` engine: init, parse, render, plugin install |
| `mdit/token.h`    | `mdit_token` and the `mdit_vec_token` containers |
| `mdit/state.h`    | `mdit_state_core/_block/_inline` parser-state types |
| `mdit/ruler.h`    | `mdit_ruler` rule registration / re-ordering |
| `mdit/renderer.h` | `mdit_renderer` and the per-token render hooks |
| `mdit/parser_core.h`<br>`mdit/parser_block.h`<br>`mdit/parser_inline.h` | each parser chain (rule signatures, registration) |
| `mdit/arena.h`    | `mdit_arena` bump allocator used by every long-lived object |
| `mdit/str.h`      | `mdit_str` (length+pointer view) and `mdit_buf` (mutable byte buffer) |
| `mdit/vec.h`      | type-generic dynamic array macros |
| `mdit/map.h`      | ordered string-keyed maps (used for token attrs) |
| `mdit/utf.h`      | UTF-8 codepoint iteration + Unicode classification |
| `mdit/url.h`<br>`mdit/normalize_url.h`<br>`mdit/punycode.h` | the `mdurl` port + IDN handling |
| `mdit/escape.h`   | `escapeHtml` / `unescapeAll` byte helpers |
| `mdit/json.h`     | `mdit_token_to_json` + low-level emitters used by the oracle |
| `mdit/linkifier.h`| `mdit_linkifier` vtable + the bundled default implementation |

The headers are generated into `${CMAKE_INSTALL_INCLUDEDIR}/mdit/`
when `MDIT_INSTALL=ON`.

## Lifecycle

```c
#include <mdit/main.h>
#include <mdit/mdit_lib_ctx.h>

mdit_lib_ctx lib;
mdit_arena   arena;
mdit_md      md;

mdit_lib_ctx_init_defaults(&lib);
mdit_arena_init(&arena, 0);
mdit_md_init(&md, &lib, &arena);

mdit_buf out;
mdit_buf_init(&out);
mdit_md_render(&md, MDIT_STR_LIT("# hi"), NULL, &out);
fwrite(out.data, 1, out.len, stdout);

mdit_buf_destroy(&out);
mdit_md_destroy(&md);
mdit_arena_destroy(&lib, &arena);
```

The engine pulls every long-lived allocation (tokens, attribute
maps, interned strings) from the user-supplied `mdit_arena`.
Allocator behavior and out-of-memory handling come from `mdit_lib_ctx`,
which is passed alongside the arena to init, reset, and destroy calls.
When the arena is destroyed, every object that ever flowed through the
engine vanishes in a single free — the simplest possible memory
model and the source of most of the C engine's speed advantage.

## Plugins

Plugins receive the live `mdit_md` and install rules through the
parser-specific `mdit_ruler`s. Mirrors the upstream Python plugin
contract one-to-one — see `docs/PLUGINS.md` for a side-by-side port
walkthrough.

## Bindings

`mdit-c/bindings/python/` ships a CPython extension (`_mdit_c` plus
the `mdit_c` Python facade) that wraps the same API and re-emits
upstream `markdown_it.Token` instances. The pytest suite under
`bindings/python/upstream_tests/` reuses the markdown-it-py test
corpus to assert byte-for-byte parity.

## Generating the HTML reference

Configure with `MDIT_BUILD_DOCS=ON` and build the `mdit_docs`
target:

```sh
cmake -S mdit-c -B mdit-c/build-docs -DMDIT_BUILD_DOCS=ON
cmake --build mdit-c/build-docs --target mdit_docs
xdg-open mdit-c/build-docs/docs/doxygen/html/index.html
```

The build skips silently if Doxygen isn't installed; the CMake
configure step prints a one-line status with the discovered version
or the "skipped" reason so it's never a hidden failure.
