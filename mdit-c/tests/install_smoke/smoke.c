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
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mdit/mdit.h"

int main(void)
{
    mdit_ctx *md = mdit_new(NULL, "commonmark");
    if (md == NULL) {
        fprintf(stderr, "mdit_new failed\n");
        return 2;
    }

    char *html = NULL;
    size_t html_len = 0;
    if (mdit_render(md, "# hi\n", SIZE_MAX, &html, &html_len) != MDIT_OK) {
        fprintf(stderr, "mdit_render failed\n");
        mdit_free(md);
        return 3;
    }

    static const char want[] = "<h1>hi</h1>\n";
    int ok = (html_len == sizeof want - 1 &&
              memcmp(html, want, html_len) == 0);

    if (!ok) {
        fprintf(stderr, "smoke: wanted %zu bytes %s, got %zu bytes %.*s\n",
                sizeof want - 1, want, html_len,
                (int)html_len, html);
    }

    mdit_free_string(md, html);
    mdit_free(md);

    fputs(ok ? "install_smoke: OK\n" : "install_smoke: FAIL\n",
          ok ? stdout : stderr);
    return ok ? 0 : 1;
}
