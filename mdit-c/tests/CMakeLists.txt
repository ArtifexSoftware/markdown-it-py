# Phase 1 unit tests for the foundation layer.
#
# Each test file becomes its own executable + ctest entry. Keeping them
# separate means a single failure in one suite doesn't mask others, and
# ctest can run them in parallel under -j.

set(MDIT_TEST_SUITES
    test_arena
    test_str
    test_vec
    test_map
    test_utf
    test_entities
    test_json
    test_token
    test_ruler
    test_url
    test_punycode
    test_escape
    test_renderer
    test_main
    test_mdit_api
    test_block_rules
    test_token_oracle
)

foreach(suite IN LISTS MDIT_TEST_SUITES)
    add_executable(${suite} ${suite}.c)
    target_link_libraries(${suite} PRIVATE mdit_static)
    target_include_directories(${suite} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    )
    target_link_libraries(${suite} PRIVATE mdit_warnings mdit_sanitizers)
    add_test(NAME ${suite} COMMAND ${suite})
    # Keep test output visible on failure, regardless of ctest verbosity.
    set_tests_properties(${suite} PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL \\["
    )
endforeach()

# AddressSanitizer rejects absurd malloc sizes before the allocator runs;
# the OOM-handler test uses a huge size and expects NULL / longjmp instead.
if(MDIT_ENABLE_ASAN)
    set_tests_properties(test_arena PROPERTIES
        ENVIRONMENT "ASAN_OPTIONS=allocator_may_return_null=1"
    )
endif()

# The token-oracle runner consumes JSONL files at runtime; tell it
# where they live (absolute path so the test works regardless of cwd).
target_compile_definitions(test_token_oracle PRIVATE
    "MDIT_TEST_ORACLE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/oracle\""
)

# ---------------------------------------------------------------------------
# install-package smoke test
#
# Exercises the install rules + CMake package config end-to-end: stage
# install -> configure a tiny consumer with `find_package(mdit)` ->
# build -> run -> assert HTML matches. The CMake driver lives in
# `install_smoke/run_smoke.cmake`.
#
# Skipped when MDIT_INSTALL=OFF (no install rules emitted) or when the
# parent generator is multi-config and we couldn't infer it cleanly.
# ---------------------------------------------------------------------------
if(MDIT_INSTALL)
    set(_smoke_stage  "${CMAKE_BINARY_DIR}/install_smoke_stage")
    set(_smoke_build  "${CMAKE_BINARY_DIR}/install_smoke_build")
    set(_smoke_source "${CMAKE_CURRENT_SOURCE_DIR}/install_smoke")
    add_test(
        NAME install_smoke
        COMMAND ${CMAKE_COMMAND}
            -DSTAGE_DIR=${_smoke_stage}
            -DBINARY_DIR=${_smoke_build}
            -DSOURCE_DIR=${_smoke_source}
            -DSUPER_BUILD_DIR=${CMAKE_BINARY_DIR}
            -DCONFIG=$<CONFIG>
            -DGENERATOR=${CMAKE_GENERATOR}
            -P "${_smoke_source}/run_smoke.cmake"
    )
    # The smoke consumer needs the full library + CLI + headers to be
    # built first. Express that as a fixture so ctest -j stays correct.
    set_tests_properties(install_smoke PROPERTIES
        LABELS "install"
    )
endif()
