cmake_minimum_required(VERSION 3.20)
project(mdit_c
    VERSION 0.0.1
    DESCRIPTION "C11 port of markdown-it-py"
    LANGUAGES C
)

# ---------------------------------------------------------------------------
# Project-wide settings
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "" FORCE)
endif()

# When building the shared library on Windows, every executable in the
# build tree (CLI, tests, install_smoke staging) needs `libmdit.dll`
# discoverable. The cleanest fix is to put every runtime artefact
# (.exe + .dll) in the same directory so the dynamic loader finds it
# without needing to fiddle with PATH on every add_test() call.
if(WIN32)
    if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    endif()
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(MDIT_BUILD_CLI       "Build the md_cli command-line tool"            ON)
option(MDIT_BUILD_TESTS     "Build the C test suite"                        ON)
option(MDIT_BUILD_PYBIND    "Build the optional CPython extension"          OFF)
option(MDIT_BUILD_SHARED    "Build mdit as a shared library (versioned)"    OFF)
option(MDIT_BUILD_FUZZ      "Build the libFuzzer-style fuzz harnesses"      OFF)
option(MDIT_FUZZ_LIBFUZZER  "Link fuzz harnesses with -fsanitize=fuzzer (Clang only)" OFF)
option(MDIT_BUILD_DOCS      "Configure the Doxygen API reference target"    OFF)
option(MDIT_INSTALL         "Generate install rules + package config"       ON)
option(MDIT_ENABLE_ASAN     "Build with AddressSanitizer (Debug builds)"    OFF)
option(MDIT_ENABLE_UBSAN    "Build with UndefinedBehaviorSanitizer"         OFF)
option(MDIT_WERROR          "Treat warnings as errors"                      OFF)

# ---------------------------------------------------------------------------
# Warning / sanitizer flags (applied to the library + CLI + tests)
# ---------------------------------------------------------------------------
add_library(mdit_warnings INTERFACE)

if(MSVC)
    target_compile_options(mdit_warnings INTERFACE /W4 /permissive-)
    if(MDIT_WERROR)
        target_compile_options(mdit_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(mdit_warnings INTERFACE
        -Wall -Wextra -Wpedantic
        -Wshadow -Wstrict-prototypes -Wmissing-prototypes
        -Wpointer-arith -Wcast-qual -Wcast-align
        -Wnull-dereference -Wdouble-promotion
        -Wformat=2
    )
    if(MDIT_WERROR)
        target_compile_options(mdit_warnings INTERFACE -Werror)
    endif()
endif()

add_library(mdit_sanitizers INTERFACE)
if(MDIT_ENABLE_ASAN AND NOT MSVC)
    target_compile_options(mdit_sanitizers INTERFACE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(mdit_sanitizers    INTERFACE -fsanitize=address)
endif()
if(MDIT_ENABLE_UBSAN AND NOT MSVC)
    target_compile_options(mdit_sanitizers INTERFACE -fsanitize=undefined -fno-omit-frame-pointer)
    target_link_options(mdit_sanitizers    INTERFACE -fsanitize=undefined)
endif()

# `GNUInstallDirs` defines `CMAKE_INSTALL_INCLUDEDIR` / `CMAKE_INSTALL_LIBDIR`
# / etc. We pull it in BEFORE the library target so the `INSTALL_INTERFACE`
# generator expressions below see the resolved values at parse time
# (these `${CMAKE_INSTALL_*DIR}` substitutions happen immediately, not at
# generate time, even though they are wrapped in `$<INSTALL_INTERFACE:...>`).
include(GNUInstallDirs)

# ---------------------------------------------------------------------------
# Library target
#
# The CMake target name `mdit_static` is kept for backward compatibility
# with every consumer (`cli/`, `tests/`, `bindings/python/`). When
# `MDIT_BUILD_SHARED=ON` it is built as a versioned SHARED library —
# the target name is then a misnomer but flipping it would ripple into
# many call sites for no real benefit.
#
# OUTPUT_NAME is `mdit` so the produced binary is `libmdit.{a,so,dylib}`
# or `mdit.{lib,dll}` regardless of kind. On Windows we ask CMake to
# auto-export every public symbol via WINDOWS_EXPORT_ALL_SYMBOLS so the
# shared variant works without having to thread `__declspec(dllexport)`
# annotations through the internal headers.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE MDIT_SRC CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
)
if(MDIT_SRC)
    if(MDIT_BUILD_SHARED)
        set(_MDIT_LIB_KIND SHARED)
    else()
        set(_MDIT_LIB_KIND STATIC)
    endif()
    add_library(mdit_static ${_MDIT_LIB_KIND} ${MDIT_SRC})
    # PUBLIC includes are wrapped in BUILD_INTERFACE / INSTALL_INTERFACE
    # generator expressions so the same target can be exported via
    # `install(EXPORT)` without CMake complaining about absolute paths
    # in the source tree.
    target_include_directories(mdit_static PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    )
    # `src/` is installed into `<prefix>/include/mdit/`, so PUBLIC
    # consumers see it from both the build tree and the install tree.
    target_include_directories(mdit_static PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/mdit>"
    )
    target_link_libraries(mdit_static PRIVATE mdit_warnings mdit_sanitizers)
    set_target_properties(mdit_static PROPERTIES
        OUTPUT_NAME mdit
        VERSION     "${PROJECT_VERSION}"
        SOVERSION   "${PROJECT_VERSION_MAJOR}"
    )
    if(MDIT_BUILD_SHARED)
        # MSVC: emit a .def-style auto-exports list so consumers see the
        # full public surface without per-symbol annotations.
        set_target_properties(mdit_static PROPERTIES
            WINDOWS_EXPORT_ALL_SYMBOLS ON
        )
        # Tell ELF/Mach-O linkers we're building a shared object so
        # any consumer including the headers compiles with consistent
        # macros (currently a no-op on POSIX, but keeps the symmetry
        # with the Windows DLL flag and makes future visibility
        # tightening trivial).
        target_compile_definitions(mdit_static PRIVATE MDIT_BUILDING)
        target_compile_definitions(mdit_static PUBLIC  MDIT_SHARED)
    endif()
endif()

# ---------------------------------------------------------------------------
# CLI / tests / bindings — wired in their own subdirectories when populated.
# ---------------------------------------------------------------------------
if(MDIT_BUILD_TESTS)
    # `enable_testing()` must run in the root CMakeLists *before* any
    # subdirectory that registers `add_test(...)` calls (cli/, tests/).
    enable_testing()
endif()

if(MDIT_BUILD_CLI AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cli/CMakeLists.txt")
    add_subdirectory(cli)
endif()

if(MDIT_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

if(MDIT_BUILD_PYBIND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bindings/python/CMakeLists.txt")
    add_subdirectory(bindings/python)
endif()

if(MDIT_BUILD_FUZZ AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/CMakeLists.txt")
    add_subdirectory(fuzz)
endif()

if(MDIT_BUILD_DOCS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/docs/CMakeLists.txt")
    add_subdirectory(docs)
endif()

# ---------------------------------------------------------------------------
# Install rules + package config
#
# When MDIT_INSTALL=ON (the default) we generate everything required for
# downstream projects to consume `mdit-c` via `find_package(mdit)` or
# `pkg-config --cflags --libs mdit`. We install:
#
#   * the `mdit` library (static by default; versioned shared when
#     `MDIT_BUILD_SHARED=ON` — installed under both ARCHIVE and LIBRARY
#     destinations to cover Windows DLL + import lib correctly),
#   * the curated public header `include/mdit/mdit.h`,
#   * the current `src/*.h` headers (relocated under `include/mdit/`)
#     so callers using the present API surface can link cleanly,
#   * the CLI executable `markdown-it-c`,
#   * a CMake package config (`mditConfig.cmake` + version file),
#   * a pkg-config file (`mdit.pc`).
#
# The `src/` headers will retract once `include/mdit/mdit.h` becomes the
# only documented surface (Phase 5+); they are installed alongside it
# today so external builds aren't blocked.
# ---------------------------------------------------------------------------
if(MDIT_INSTALL AND TARGET mdit_static)
    include(CMakePackageConfigHelpers)

    set(MDIT_INSTALL_CMAKEDIR
        "${CMAKE_INSTALL_LIBDIR}/cmake/mdit"
        CACHE STRING "Install path for mditConfig.cmake")
    set(MDIT_INSTALL_PKGCONFIGDIR
        "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
        CACHE STRING "Install path for mdit.pc")

    install(TARGETS mdit_static mdit_warnings mdit_sanitizers
        EXPORT mditTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        INCLUDES DESTINATION
            "${CMAKE_INSTALL_INCLUDEDIR}"
            "${CMAKE_INSTALL_INCLUDEDIR}/mdit"
    )

    # Curated public header — one per file under include/mdit/.
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/mdit"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        FILES_MATCHING PATTERN "*.h")

    # Internal headers (current API surface). They are installed under
    # `<prefix>/include/mdit/` so callers do `#include "main.h"` etc.
    # with `-I<prefix>/include/mdit`.
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/mdit"
        FILES_MATCHING PATTERN "*.h")

    # Optional: install the CLI binary. The CMake target only exists
    # when MDIT_BUILD_CLI=ON, so we guard on the target.
    if(TARGET md_cli)
        install(TARGETS md_cli
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    endif()

    install(EXPORT mditTargets
        FILE        mditTargets.cmake
        NAMESPACE   mdit::
        DESTINATION "${MDIT_INSTALL_CMAKEDIR}")

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/mditConfigVersion.cmake"
        VERSION       "${PROJECT_VERSION}"
        COMPATIBILITY SameMinorVersion)

    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/mditConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/mditConfig.cmake"
        INSTALL_DESTINATION "${MDIT_INSTALL_CMAKEDIR}")

    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/mditConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/mditConfigVersion.cmake"
        DESTINATION "${MDIT_INSTALL_CMAKEDIR}")

    # pkg-config — substitutes prefix/version/etc. into the template.
    set(MDIT_PC_PREFIX      "${CMAKE_INSTALL_PREFIX}")
    set(MDIT_PC_LIBDIR      "${CMAKE_INSTALL_FULL_LIBDIR}")
    set(MDIT_PC_INCLUDEDIR  "${CMAKE_INSTALL_FULL_INCLUDEDIR}")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/mdit.pc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/mdit.pc" @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/mdit.pc"
        DESTINATION "${MDIT_INSTALL_PKGCONFIGDIR}")
endif()

# ---------------------------------------------------------------------------
# Phase 0 status banner
# ---------------------------------------------------------------------------
message(STATUS "mdit-c ${PROJECT_VERSION} configuring (Phase 0: skeleton)")
