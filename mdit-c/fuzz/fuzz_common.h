/*
 * fuzz_common.h — shared scaffolding for libFuzzer-style harnesses.
 *
 * Each harness defines:
 *
 *   int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
 *
 * — the standard libFuzzer entrypoint. When the harness is built with
 * `clang -fsanitize=fuzzer` it links libFuzzer's own `main`, which
 * drives the function with mutation-driven inputs and reports crashes.
 *
 * When libFuzzer isn't available (the default for the in-tree CTest
 * smoke build) the harness instead links the standalone `main` defined
 * in ``fuzz_main.c`` below, which simply iterates over the files
 * supplied on the command line and replays each one through
 * ``LLVMFuzzerTestOneInput``. That gives us a regression-style replay
 * harness that runs in seconds and works on every supported compiler.
 */
#ifndef MDIT_FUZZ_COMMON_H
#define MDIT_FUZZ_COMMON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* MDIT_FUZZ_COMMON_H */
