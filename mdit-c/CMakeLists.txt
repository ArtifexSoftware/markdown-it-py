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

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(MDIT_BUILD_CLI       "Build the md_cli command-line tool"            ON)
option(MDIT_BUILD_TESTS     "Build the C test suite"                        ON)
option(MDIT_BUILD_PYBIND    "Build the optional CPython extension"          OFF)
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

# ---------------------------------------------------------------------------
# Library target (Phase 0: header-only placeholder; Phase 1 fills src/)
# ---------------------------------------------------------------------------
add_library(mdit INTERFACE)
target_include_directories(mdit INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(mdit INTERFACE mdit_warnings mdit_sanitizers)

# Sources will be wired up in Phase 1. Keeping the placeholder here makes the
# CMake graph valid today and the diff smaller when implementation lands.
file(GLOB_RECURSE MDIT_SRC CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
)
if(MDIT_SRC)
    add_library(mdit_static STATIC ${MDIT_SRC})
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
    set_target_properties(mdit_static PROPERTIES OUTPUT_NAME mdit)
endif()

# `GNUInstallDirs` is needed to define `CMAKE_INSTALL_INCLUDEDIR` / etc.
# for the generator-expression PATH above. We pull it in unconditionally
# so the path expansion is always valid; the install rules themselves
# are still gated below by `MDIT_INSTALL`.
include(GNUInstallDirs)

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

# ---------------------------------------------------------------------------
# Install rules + package config
#
# When MDIT_INSTALL=ON (the default) we generate everything required for
# downstream projects to consume `mdit-c` via `find_package(mdit)` or
# `pkg-config --cflags --libs mdit`. We install:
#
#   * the `mdit` static library (PUBLIC headers visible),
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
