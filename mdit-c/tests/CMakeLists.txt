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

# The token-oracle runner consumes JSONL files at runtime; tell it
# where they live (absolute path so the test works regardless of cwd).
target_compile_definitions(test_token_oracle PRIVATE
    "MDIT_TEST_ORACLE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/oracle\""
)
