# Fuzz harnesses for the mdit-c engine.
#
# Each harness compiles to an executable that exposes the standard
# libFuzzer entrypoint ``LLVMFuzzerTestOneInput``. Two link modes are
# supported:
#
#   * Coverage-guided fuzzing (clang only):
#         cmake -S mdit-c -B build-fuzz \
#               -DMDIT_BUILD_FUZZ=ON -DMDIT_FUZZ_LIBFUZZER=ON \
#               -DCMAKE_C_COMPILER=clang
#         cmake --build build-fuzz
#         ./build-fuzz/fuzz/fuzz_parse_render corpus/parse_render
#     Links the harness with ``-fsanitize=fuzzer,address`` so libFuzzer
#     supplies ``main`` and AddressSanitizer catches issues.
#
#   * Replay smoke (any C99 compiler — the default):
#         cmake -S mdit-c -B build -DMDIT_BUILD_FUZZ=ON
#         cmake --build build
#         ctest --test-dir build -R fuzz_smoke
#     Links the standalone ``fuzz_main.c`` driver, which iterates over
#     a directory of regression inputs and replays each through
#     ``LLVMFuzzerTestOneInput``. Used by the in-tree CTest entry so
#     the fuzz corpus stays exercised on every CI run, even without a
#     clang+libFuzzer toolchain.
#
# When MDIT_BUILD_FUZZ=OFF (the default) this directory is skipped,
# so a vanilla ``cmake --build`` doesn't pay any extra build cost.

set(MDIT_FUZZ_HARNESSES
    fuzz_parse_render
    fuzz_inline
    fuzz_token_roundtrip
)

foreach(harness IN LISTS MDIT_FUZZ_HARNESSES)
    set(_srcs ${harness}.c)
    if(NOT MDIT_FUZZ_LIBFUZZER)
        # Standalone replay driver supplies main(); add it once per
        # harness so each binary is self-contained.
        list(APPEND _srcs fuzz_main.c)
    endif()

    add_executable(${harness} ${_srcs})
    target_link_libraries(${harness} PRIVATE mdit_static
                                              mdit_warnings
                                              mdit_sanitizers)
    target_include_directories(${harness} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    )
    if(MSVC)
        # The replay driver uses ``strerror`` for legible error
        # reporting; MSVC's CRT marks it deprecated. We don't ship
        # the harnesses, so silence the warning rather than churning
        # the code with a ``strerror_s`` shim.
        target_compile_definitions(${harness} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()

    if(MDIT_FUZZ_LIBFUZZER)
        if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang"
           AND NOT CMAKE_C_COMPILER_ID STREQUAL "AppleClang")
            message(FATAL_ERROR
                "MDIT_FUZZ_LIBFUZZER=ON requires the Clang C compiler "
                "(libFuzzer is part of compiler-rt).")
        endif()
        # ``-fsanitize=fuzzer`` provides libFuzzer's main + the
        # mutation/coverage instrumentation. Pair it with ASan + UBSan
        # to surface the most common classes of bugs.
        target_compile_options(${harness} PRIVATE
            -fsanitize=fuzzer-no-link,address,undefined
            -fno-omit-frame-pointer)
        target_link_options(${harness} PRIVATE
            -fsanitize=fuzzer,address,undefined)
    endif()
endforeach()

# ---------------------------------------------------------------------------
# CTest smoke entry: replay the seeded corpus through every harness.
#
# This is a regression check, not real fuzzing — it runs in well under
# a second per harness and exists so corpus inputs that have crashed
# the engine in the past stay green on every CI run. Coverage-guided
# fuzzing is left to oss-fuzz / out-of-band invocations.
# ---------------------------------------------------------------------------
if(NOT MDIT_FUZZ_LIBFUZZER)
    foreach(harness IN LISTS MDIT_FUZZ_HARNESSES)
        # Map "fuzz_parse_render" -> "parse_render", "fuzz_inline" ->
        # "inline", etc., to find the matching corpus subdirectory.
        string(REGEX REPLACE "^fuzz_" "" _kind "${harness}")
        set(_corpus "${CMAKE_CURRENT_SOURCE_DIR}/corpus/${_kind}")
        if(EXISTS "${_corpus}")
            add_test(
                NAME fuzz_smoke_${_kind}
                COMMAND $<TARGET_FILE:${harness}> "${_corpus}"
            )
            set_tests_properties(fuzz_smoke_${_kind} PROPERTIES
                LABELS "fuzz"
                PASS_REGULAR_EXPRESSION
                    "fuzz: replayed [0-9]+ input\\(s\\) OK"
            )
        endif()
    endforeach()
endif()
