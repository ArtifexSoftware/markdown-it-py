/*
 * install_smoke/smoke.c — verifies the installed package config / pkg-config
 * is sufficient to build a minimal `markdown-it` consumer. It does NOT
 * exercise the parser exhaustively (the in-tree CTest suites do that):
 * its job is just "compile + link + run + render `# hi` correctly".
 *
 * The smoke test is built out-of-tree against an already-staged install
 * (see scripts/test_install.cmake or the README). Running it is gated
 * on a successful `cmake --install`.
 */
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "arena.h"
#include "mdit/mdit_lib_ctx.h"
#include "str.h"

int main(void)
{
    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena a;
    mdit_arena_init(&a, 0);

    mdit_md md;
    if (!mdit_md_init(&md, &lib, &a)) {
        fprintf(stderr, "mdit_md_init failed\n");
        return 2;
    }

    mdit_buf out;
    mdit_buf_init(&out);
    mdit_str src = MDIT_STR_LIT("# hi\n");
    if (!mdit_md_render(&md, src, NULL, &out)) {
        fprintf(stderr, "mdit_md_render failed\n");
        return 3;
    }

    static const char want[] = "<h1>hi</h1>\n";
    int ok = (out.len == sizeof want - 1 &&
              memcmp(out.data, want, out.len) == 0);

    if (!ok) {
        fprintf(stderr, "smoke: wanted %zu bytes %s, got %zu bytes %.*s\n",
                sizeof want - 1, want, out.len,
                (int)out.len, (const char *)out.data);
    }

    mdit_buf_destroy(&out);
    mdit_md_destroy(&md);
    mdit_arena_destroy(&lib, &a);

    fputs(ok ? "install_smoke: OK\n" : "install_smoke: FAIL\n",
          ok ? stdout : stderr);
    return ok ? 0 : 1;
}
