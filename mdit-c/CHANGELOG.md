# Changelog — mdit-c

All notable changes to the C99 port are documented here. The project is
still pre–1.0; expect API adjustments until Phase 5.

## Unreleased

### Breaking

- **Explicit library context (`mdit_lib_ctx`)**: Allocator hooks and the
  out-of-memory callback are no longer stored on `mdit_arena`. Call
  `mdit_lib_ctx_init_defaults()` (or fill a `mdit_lib_ctx` yourself), then
  pass that pointer to `mdit_md_init(md, lib, arena)`, to
  `mdit_arena_alloc` / `zalloc` / `reset` / `destroy`, and to other APIs
  that allocate or free arena-backed data (vectors, maps, tokens, URL/IDN
  helpers, linkifier paths, etc.).
- **`mdit_md_init`** now takes `(mdit_md *, mdit_lib_ctx *, mdit_arena *)`.
- **`mdit_vec_token_init`** and related container inits take
  `(…, mdit_lib_ctx *, mdit_arena *)`.
