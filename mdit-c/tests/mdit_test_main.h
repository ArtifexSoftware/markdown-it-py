/*
 * mdit_test_main.h — emits the runner main() for a single test TU.
 *
 * Include this *exactly once* at the bottom of each test .c file, after
 * defining the registry with MDIT_TEST_REGISTRY(...). It is _not_ a
 * normal header in the include-once sense: it intentionally compiles
 * the symbols it declares so that one .c file == one ctest executable.
 */
#ifdef MDIT_TEST_MAIN_INCLUDED
#  error "mdit_test_main.h must only be included once per translation unit"
#endif
#define MDIT_TEST_MAIN_INCLUDED 1

#include <inttypes.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mdit_test.h"

#ifndef MDIT_TEST_REGISTRY
#  error "Define MDIT_TEST_REGISTRY(...) listing every MDIT_TEST(name) above"
#endif

jmp_buf      mdit_test_jmp;
int          mdit_test_failed_count = 0;
const char  *mdit_test_current_name = NULL;

void mdit_test_fail(const char *file, int line, const char *msg)
{
    fprintf(stderr,
            "  FAIL [%s] %s:%d: %s\n",
            mdit_test_current_name ? mdit_test_current_name : "(null)",
            file, line, msg ? msg : "(no message)");
    longjmp(mdit_test_jmp, 1);
}

const mdit_test_case mdit_test_registry[] = {
    MDIT_TEST_REGISTRY
};
const size_t mdit_test_registry_len =
    sizeof(mdit_test_registry) / sizeof(mdit_test_registry[0]);

int main(int argc, char **argv)
{
    const char *only = NULL;
    if (argc >= 2) {
        only = argv[1]; /* run a single named test, useful for debugging */
    }

    size_t ran = 0;
    size_t failed = 0;
    for (size_t i = 0; i < mdit_test_registry_len; ++i) {
        const mdit_test_case *t = &mdit_test_registry[i];
        if (only != NULL && strcmp(only, t->name) != 0) {
            continue;
        }
        mdit_test_current_name = t->name;
        ++ran;
        if (setjmp(mdit_test_jmp) == 0) {
            t->run();
            fprintf(stdout, "  ok    %s\n", t->name);
        } else {
            ++failed;
        }
        mdit_test_current_name = NULL;
    }

    if (only != NULL && ran == 0) {
        fprintf(stderr, "no test named %s\n", only);
        return 2;
    }

    fprintf(stdout, "summary: %zu run, %zu failed\n", ran, failed);
    return failed == 0 ? 0 : 1;
}
