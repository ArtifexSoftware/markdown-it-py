# mdit-c CPython extension build.
#
# Builds a small `_mdit_c` module that exposes the C engine to Python.
# Activated by `MDIT_BUILD_PYBIND=ON` (default OFF). Skipped silently
# when no Python3 development headers are available, so a Python-less
# checkout (or a CI host without the headers) still configures cleanly.

if(NOT TARGET mdit_static)
    message(STATUS "mdit-c Python bindings skipped: static library not configured")
    return()
endif()

find_package(Python3 COMPONENTS Interpreter Development.Module)
if(NOT Python3_Development.Module_FOUND)
    message(WARNING
        "mdit-c Python bindings requested but Python3 development headers "
        "were not found; skipping. Set MDIT_BUILD_PYBIND=OFF to silence.")
    return()
endif()

# The extension module: `_mdit_c` (underscore prefix marks it as
# internal — end-users import from the `mdit_c` package wrapper).
Python3_add_library(_mdit_c MODULE WITH_SOABI _mdit_c.c)
set_target_properties(_mdit_c PROPERTIES
    OUTPUT_NAME _mdit_c
    PREFIX ""
)

target_link_libraries(_mdit_c PRIVATE
    mdit_static
    mdit_warnings
    mdit_sanitizers
)

# `_mdit_c.c` deliberately reaches into the impl headers (main.h /
# arena.h / str.h) — the public surface in `include/mdit/mdit.h` will
# eventually grow the equivalent wrappers, at which point we can
# narrow this include path.
target_include_directories(_mdit_c PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
)

if(MSVC)
    target_compile_definitions(_mdit_c PRIVATE _CRT_SECURE_NO_WARNINGS)
    # CPython's headers (Py_TPFLAGS_DEFAULT, PyType_GenericNew, etc.)
    # trip several /W4-and-higher warnings that we treat as errors in
    # the rest of the project. Disable them here so the extension can
    # still be built with the same warning flags as the static lib.
    #   4100 unreferenced formal parameter
    #   4127 conditional expression is constant
    #   4232 dllimport address used as static initializer
    #   4244 conversion: possible loss of data
    #   4267 conversion: size_t -> smaller
    target_compile_options(_mdit_c PRIVATE
        /wd4100 /wd4127 /wd4232 /wd4244 /wd4267)
endif()

# Stage the extension + the `mdit_c` package wrapper next to each
# other so a smoke test can `import mdit_c` directly. The pure-Python
# package lives in `mdit_c/` already; we just copy the built module
# alongside it on each build.
set(MDIT_PY_STAGE "${CMAKE_BINARY_DIR}/python_stage")
add_custom_command(TARGET _mdit_c POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MDIT_PY_STAGE}/mdit_c"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/mdit_c"
            "${MDIT_PY_STAGE}/mdit_c"
    COMMAND ${CMAKE_COMMAND} -E copy
            "$<TARGET_FILE:_mdit_c>"
            "${MDIT_PY_STAGE}/mdit_c/$<TARGET_FILE_NAME:_mdit_c>"
    VERBATIM
)

# ---------------------------------------------------------------------
# Smoke test — runs the extension under the host Python and asserts
# byte-parity with `markdown_it.MarkdownIt` for a small bank of inputs.
# Auto-skipped when the upstream Python package isn't importable in
# the test interpreter (the script handles the skip itself).
# ---------------------------------------------------------------------
if(MDIT_BUILD_TESTS)
    add_test(
        NAME python_smoke
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/test_smoke.py"
                "${MDIT_PY_STAGE}"
    )
    set_tests_properties(python_smoke PROPERTIES
        LABELS "python"
    )

    # ----------------------------------------------------------------
    # Upstream-pytest harness — runs the upstream test corpus
    # (CommonMark spec + tests/test_port/fixtures + a curated subset
    # of tests/test_port) against the C engine. The driver script
    # exits 0 with a SKIP marker when ``pytest`` or ``markdown_it``
    # aren't importable, so dependency-light hosts still pass.
    # ----------------------------------------------------------------
    add_test(
        NAME python_pytest
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/run_upstream_pytest.py"
                "${MDIT_PY_STAGE}"
    )
    set_tests_properties(python_pytest PROPERTIES
        LABELS "python"
    )
endif()
