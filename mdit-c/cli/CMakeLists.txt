# mdit-c command-line tool.
#
# The CLI today depends on the *internal* parser/render entry points
# (`mdit_md_*`) declared in `../src/main.h`. Once `include/mdit/mdit.h`
# grows real implementations for `mdit_new` / `mdit_render`, the CLI
# can move onto the public surface and we'll drop the private include
# path below.

if(NOT TARGET mdit_static)
    message(STATUS "mdit-c CLI skipped: static library target not configured")
    return()
endif()

add_executable(md_cli md_cli.c)
set_target_properties(md_cli PROPERTIES OUTPUT_NAME markdown-it-c)

# Pull in the internal headers (main.h, arena.h, json.h, str.h, …).
target_include_directories(md_cli PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)

target_link_libraries(md_cli PRIVATE
    mdit_static
    mdit_warnings
    mdit_sanitizers
)

if(MSVC)
    # Quieter Windows runtime: we never call the deprecated stdio
    # variants the CRT warns about by default (fopen, fread, etc.).
    target_compile_definitions(md_cli PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---------------------------------------------------------------------
# CLI smoke tests (only registered when the test suite is enabled).
# Heavyweight HTML parity is covered by `test_block_rules` and
# `test_token_oracle` against the library directly; here we just
# confirm the executable starts, parses argv, and reports versions.
# ---------------------------------------------------------------------
if(MDIT_BUILD_TESTS)
    add_test(NAME md_cli_version COMMAND md_cli --version)
    set_tests_properties(md_cli_version PROPERTIES
        PASS_REGULAR_EXPRESSION "markdown-it-c [0-9]+\\.[0-9]+\\.[0-9]+")

    add_test(NAME md_cli_help COMMAND md_cli --help)
    set_tests_properties(md_cli_help PROPERTIES
        PASS_REGULAR_EXPRESSION "usage: markdown-it-c")

    add_test(NAME md_cli_unknown_option COMMAND md_cli --no-such-flag)
    set_tests_properties(md_cli_unknown_option PROPERTIES
        WILL_FAIL TRUE)

    # Byte-parity sweep: ``markdown-it-c`` vs ``python -m
    # markdown_it.cli.parse`` over a small but representative bank of
    # inputs. Skipped automatically when no Python 3 interpreter is
    # available (e.g. minimal CI containers); the unit suites still
    # cover the underlying library.
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME md_cli_python_parity
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/test_cli_parity.py"
                "$<TARGET_FILE:md_cli>"
        )
    endif()
endif()
