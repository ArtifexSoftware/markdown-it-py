/*
 * md_cli.c — `markdown-it-c` command-line driver.
 *
 * Mirrors `markdown_it/cli/parse.py` for batch and stdin modes:
 *
 *   markdown-it-c [options] [FILE ...]
 *
 *   Options:
 *     -h, --help         Show usage and exit.
 *     -v, --version      Show version and exit.
 *     --stdin            Read Markdown from standard input even if files
 *                        are given.
 *     --                 End of options; remaining args are filenames.
 *
 * Mode selection follows upstream:
 *   - With filenames: each is rendered in order, output concatenated to
 *     stdout.
 *   - With --stdin or no filenames at all: read all of stdin and render.
 *
 * The interactive REPL the Python CLI offers is intentionally omitted
 * here; the C tool is meant for shells and pipelines, where ``--stdin``
 * is the natural fit. We can add a REPL slice later if there's demand.
 *
 * Exit codes:
 *   0  success
 *   1  cannot open / read an input file (matches upstream's wording)
 *   2  parse / render returned an error
 *   64 usage error (unknown option, etc.) — matches sysexits.h EX_USAGE
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "json.h"
#include "main.h"
#include "mdit/mdit_lib_ctx.h"
#include "ruler.h"
#include "str.h"

#define MD_CLI_PROG_NAME "markdown-it-c"

/* mdit currently has no public version macro that's stable; expose what
 * the internal headers do know. The header field is intended to evolve
 * with the public API; for now mirror it byte-for-byte. */
#define MD_CLI_VERSION_STRING MD_CLI_PROG_NAME " 0.0.1"

static void print_usage(FILE *out)
{
    fprintf(out,
        "usage: " MD_CLI_PROG_NAME " [options] [FILE ...]\n"
        "\n"
        "Parse one or more Markdown files, convert each to HTML, and print\n"
        "the result to stdout.\n"
        "\n"
        "Options:\n"
        "  -h, --help         show this help message and exit\n"
        "  -v, --version      show program version and exit\n"
        "      --stdin        read Markdown from standard input\n"
        "      --             end of options; remaining args are filenames\n"
        "\n"
        "If no filenames are given and stdin is connected, " MD_CLI_PROG_NAME "\n"
        "reads Markdown from stdin (same as ``--stdin``).\n"
        "\n"
        "Examples:\n"
        "  " MD_CLI_PROG_NAME " README.md > index.html\n"
        "  cat doc.md | " MD_CLI_PROG_NAME " --stdin\n");
}

/* ---------------------------------------------------------------------
 * Slurp helpers — read an entire file or all of stdin into a malloc'd
 * buffer. ``*out_data`` / ``*out_len`` are set on success; the caller
 * frees ``*out_data``. The buffer is *not* NUL-terminated (the parser
 * works on length-bounded views).
 * ------------------------------------------------------------------- */
static int slurp_file(const char *path, char **out_data, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, MD_CLI_PROG_NAME ": cannot open file \"%s\".\n", path);
        return 1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        fprintf(stderr, MD_CLI_PROG_NAME ": cannot seek file \"%s\".\n", path);
        return 1;
    }
    long n_signed = ftell(f);
    if (n_signed < 0) {
        fclose(f);
        fprintf(stderr, MD_CLI_PROG_NAME ": cannot tell file \"%s\".\n", path);
        return 1;
    }
    size_t n = (size_t)n_signed;
    rewind(f);

    char *buf = (n > 0) ? (char *)malloc(n) : (char *)malloc(1);
    if (buf == NULL) {
        fclose(f);
        fprintf(stderr, MD_CLI_PROG_NAME ": out of memory reading \"%s\".\n", path);
        return 1;
    }
    size_t r = (n > 0) ? fread(buf, 1, n, f) : 0;
    fclose(f);
    if (r != n) {
        free(buf);
        fprintf(stderr, MD_CLI_PROG_NAME ": short read on \"%s\".\n", path);
        return 1;
    }
    *out_data = buf;
    *out_len  = n;
    return 0;
}

static int slurp_stdin(char **out_data, size_t *out_len)
{
    /* Stdin can be a pipe, a redirect, or a real file — always read in
     * 64 KiB chunks and grow the buffer geometrically. The parser
     * doesn't care about line endings; CR/CRLF normalization happens
     * inside the core ``normalize`` rule. */
    size_t cap = 0;
    size_t len = 0;
    char  *buf = NULL;
    enum { CHUNK = 64u * 1024u };

    for (;;) {
        if (len + CHUNK > cap) {
            size_t new_cap = (cap == 0) ? CHUNK : cap * 2;
            while (new_cap < len + CHUNK) new_cap *= 2;
            char *grown = (char *)realloc(buf, new_cap);
            if (grown == NULL) {
                free(buf);
                fprintf(stderr, MD_CLI_PROG_NAME
                        ": out of memory reading stdin.\n");
                return 1;
            }
            buf = grown;
            cap = new_cap;
        }
        size_t r = fread(buf + len, 1, CHUNK, stdin);
        len += r;
        if (r < CHUNK) {
            if (ferror(stdin)) {
                free(buf);
                fprintf(stderr, MD_CLI_PROG_NAME
                        ": cannot read from standard input.\n");
                return 1;
            }
            break;  /* EOF */
        }
    }

    if (buf == NULL) {
        /* Empty stdin — return a 1-byte empty buffer so callers can
         * unconditionally pass through to mdit_md_render. */
        buf = (char *)malloc(1);
        if (buf == NULL) return 1;
    }
    *out_data = buf;
    *out_len  = len;
    return 0;
}

/* ---------------------------------------------------------------------
 * Render one input. Caller supplies an already-initialised mdit_md and
 * arena; we reset the arena between calls so multi-file invocations
 * don't accumulate memory across documents.
 * ------------------------------------------------------------------- */
static int render_one(mdit_md   *md,
                      mdit_arena *arena,
                      const char *src_data,
                      size_t      src_len,
                      const char *origin_label)
{
    mdit_buf out;
    mdit_buf_init(&out);

    mdit_str src = { src_data, src_len };
    bool ok = mdit_md_render(md, src, NULL, &out);
    if (!ok) {
        fprintf(stderr, MD_CLI_PROG_NAME
                ": render failed for %s.\n", origin_label);
        mdit_buf_destroy(&out);
        return 2;
    }

    if (out.len > 0) {
        size_t w = fwrite(out.data, 1, out.len, stdout);
        if (w != out.len) {
            fprintf(stderr, MD_CLI_PROG_NAME
                    ": short write to stdout.\n");
            mdit_buf_destroy(&out);
            return 2;
        }
    }
    mdit_buf_destroy(&out);

    /* Reset the arena for the next file. ``mdit_md`` keeps borrowed
     * pointers into the arena (for built-in rule names etc.) so we
     * cannot reset between renders without re-init; callers that want
     * a fresh arena should use one ``mdit_md`` per file. The current
     * setup keeps the arena live for the program's lifetime. */
    (void)arena;
    return 0;
}

/* ---------------------------------------------------------------------
 * Argument parsing
 * ------------------------------------------------------------------- */
typedef struct {
    bool        force_stdin;
    bool        stop_options;
    int         exit_code;     /* set when -h / -v / usage error */
    const char *errmsg;        /* unknown-option text on usage error */
    /* For convenience we track the slice of argv that holds positional
     * args. Both indices are inclusive of pos_first, exclusive of
     * pos_last (i.e. C half-open range). */
    int         pos_first;
    int         pos_last;
} cli_args;

static cli_args parse_args(int argc, char **argv)
{
    cli_args a = { 0 };
    a.exit_code  = -1;        /* -1 = "keep going" */
    a.pos_first  = argc;      /* default: empty range */
    a.pos_last   = argc;

    /* Single forward pass: every positional arg is appended onto the
     * range; options are interpreted and removed by skipping. */
    int write = 1;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];

        if (a.stop_options || arg[0] != '-' || arg[1] == '\0') {
            argv[write++] = argv[i];
            continue;
        }
        if (strcmp(arg, "--") == 0) {
            a.stop_options = true;
            continue;
        }
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            print_usage(stdout);
            a.exit_code = 0;
            return a;
        }
        if (strcmp(arg, "-v") == 0 || strcmp(arg, "--version") == 0) {
            puts(MD_CLI_VERSION_STRING);
            a.exit_code = 0;
            return a;
        }
        if (strcmp(arg, "--stdin") == 0) {
            a.force_stdin = true;
            continue;
        }
        a.errmsg    = arg;
        a.exit_code = 64;
        return a;
    }
    a.pos_first = 1;
    a.pos_last  = write;
    return a;
}

/* ---------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    cli_args args = parse_args(argc, argv);
    if (args.exit_code == 64) {
        fprintf(stderr, MD_CLI_PROG_NAME
                ": unknown option `%s`. Try `--help`.\n", args.errmsg);
        return 64;
    }
    if (args.exit_code != -1) return args.exit_code;

    int n_files = args.pos_last - args.pos_first;
    bool use_stdin = args.force_stdin || n_files == 0;

    mdit_lib_ctx lib;
    mdit_lib_ctx_init_defaults(&lib);
    mdit_arena arena;
    mdit_arena_init(&arena, 0);
    mdit_md md;
    if (!mdit_md_init(&md, &lib, &arena)) {
        fprintf(stderr, MD_CLI_PROG_NAME ": failed to initialize parser.\n");
        mdit_arena_destroy(&lib, &arena);
        return 2;
    }

    /* Match Python's `MarkdownIt()` defaults, which load the
     * ``commonmark`` preset: `xhtmlOut=True`, `html=True`,
     * `maxNesting=20`, and the GFM `table` rule disabled. The C port's
     * `mdit_md_init` defaults track the more conservative `default`
     * preset; we patch the differences explicitly here so the CLI is
     * byte-compatible with `python -m markdown_it.cli.parse`. */
    md.options.max_nesting = 20;
    md.options.html        = true;
    md.options.xhtml_out   = true;
    {
        mdit_str table_name  = MDIT_STR_LIT("table");
        mdit_str strike_name = MDIT_STR_LIT("strikethrough");
        (void)mdit_ruler_disable(md.block.ruler,    &table_name,  1, true);
        (void)mdit_ruler_disable(md.inline_p.ruler, &strike_name, 1, true);
        (void)mdit_ruler_disable(md.inline_p.ruler2, &strike_name, 1, true);
    }

    int exit_code = 0;

    if (n_files > 0) {
        for (int i = args.pos_first; i < args.pos_last; ++i) {
            char  *data = NULL;
            size_t len  = 0;
            int rc = slurp_file(argv[i], &data, &len);
            if (rc != 0) { exit_code = rc; break; }

            rc = render_one(&md, &arena, data, len, argv[i]);
            free(data);
            if (rc != 0) { exit_code = rc; break; }
        }
    }

    if (exit_code == 0 && use_stdin) {
        char  *data = NULL;
        size_t len  = 0;
        int rc = slurp_stdin(&data, &len);
        if (rc != 0) {
            exit_code = rc;
        } else {
            rc = render_one(&md, &arena, data, len, "<stdin>");
            free(data);
            if (rc != 0) exit_code = rc;
        }
    }

    mdit_md_destroy(&md);
    mdit_arena_destroy(&lib, &arena);
    return exit_code;
}
