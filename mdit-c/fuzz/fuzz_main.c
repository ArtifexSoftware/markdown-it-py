/*
 * fuzz_main.c — standalone replay driver.
 *
 * Linked into the harness when libFuzzer is unavailable. Treats every
 * non-flag argument as a path to an input file; reads each file into
 * memory, calls ``LLVMFuzzerTestOneInput`` once per file, and exits 0
 * if every replay completed without crashing. The harness binary
 * therefore doubles as a regression check that can be wired into
 * CTest without a clang-fuzzer toolchain.
 *
 * If a directory is passed instead of a file, all regular files in
 * the directory (non-recursive) are replayed in lexicographic order.
 *
 * Exit codes:
 *   0 — all inputs replayed successfully.
 *   1 — usage error (no inputs supplied).
 *   2 — failed to open / read an input.
 *
 * A successful run prints ``fuzz: replayed N input(s) OK`` so the
 * CTest entry can grep for completion.
 */
#include "fuzz_common.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#  include <io.h>
#  include <windows.h>
#else
#  include <dirent.h>
#endif

static int read_file(const char *path, uint8_t **out_buf, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "fuzz: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    uint8_t *buf = (uint8_t *)malloc((size_t)size + 1);
    if (buf == NULL) { fclose(f); return -1; }
    if (size > 0 && fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    buf[size] = '\0';
    *out_buf = buf;
    *out_len = (size_t)size;
    return 0;
}

static int replay_file(const char *path)
{
    uint8_t *buf = NULL;
    size_t   len = 0;
    if (read_file(path, &buf, &len) < 0) return -1;
    int rc = LLVMFuzzerTestOneInput(buf, len);
    free(buf);
    return rc == 0 ? 0 : -1;
}

/* Sort helper for the directory walk so test runs are deterministic. */
static int qsort_str_cmp(const void *a, const void *b)
{
    const char *aa = *(const char *const *)a;
    const char *bb = *(const char *const *)b;
    return strcmp(aa, bb);
}

#if defined(_WIN32)
static int is_directory(const char *path)
{
    DWORD attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) return 0;
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
}

static int replay_directory(const char *dir, size_t *count)
{
    char pattern[1024];
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "fuzz: cannot list %s\n", dir);
        return -1;
    }
    char **paths = NULL;
    size_t cap = 0, n = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        size_t need = strlen(dir) + 1 + strlen(fd.cFileName) + 1;
        char *p = (char *)malloc(need);
        if (p == NULL) { FindClose(h); return -1; }
        snprintf(p, need, "%s\\%s", dir, fd.cFileName);
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            char **np = (char **)realloc(paths, cap * sizeof(char *));
            if (np == NULL) { free(p); FindClose(h); return -1; }
            paths = np;
        }
        paths[n++] = p;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (n > 1) qsort(paths, n, sizeof(char *), qsort_str_cmp);
    int rc = 0;
    for (size_t i = 0; i < n; ++i) {
        if (replay_file(paths[i]) < 0) rc = -1;
        free(paths[i]);
        ++*count;
    }
    free(paths);
    return rc;
}
#else
static int is_directory(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

static int replay_directory(const char *dir, size_t *count)
{
    DIR *d = opendir(dir);
    if (d == NULL) {
        fprintf(stderr, "fuzz: cannot open dir %s: %s\n", dir, strerror(errno));
        return -1;
    }
    char **paths = NULL;
    size_t cap = 0, n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        size_t need = strlen(dir) + 1 + strlen(ent->d_name) + 1;
        char *p = (char *)malloc(need);
        if (p == NULL) { closedir(d); return -1; }
        snprintf(p, need, "%s/%s", dir, ent->d_name);
        if (is_directory(p)) { free(p); continue; }
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            char **np = (char **)realloc(paths, cap * sizeof(char *));
            if (np == NULL) { free(p); closedir(d); return -1; }
            paths = np;
        }
        paths[n++] = p;
    }
    closedir(d);
    if (n > 1) qsort(paths, n, sizeof(char *), qsort_str_cmp);
    int rc = 0;
    for (size_t i = 0; i < n; ++i) {
        if (replay_file(paths[i]) < 0) rc = -1;
        free(paths[i]);
        ++*count;
    }
    free(paths);
    return rc;
}
#endif

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
            "usage: %s <input-file-or-dir> [more-paths ...]\n"
            "\n"
            "Replays each input through LLVMFuzzerTestOneInput. Used\n"
            "for regression replay; for true coverage-guided fuzzing,\n"
            "rebuild with clang -fsanitize=fuzzer.\n",
            argv[0]);
        return 1;
    }
    size_t count = 0;
    int rc = 0;
    for (int i = 1; i < argc; ++i) {
        const char *p = argv[i];
        if (is_directory(p)) {
            if (replay_directory(p, &count) < 0) rc = 2;
        } else {
            if (replay_file(p) < 0) rc = 2;
            ++count;
        }
    }
    if (rc == 0) {
        printf("fuzz: replayed %zu input(s) OK\n", count);
    }
    return rc;
}
