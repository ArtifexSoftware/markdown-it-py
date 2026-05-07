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
    target_include_directories(mdit_static PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_include_directories(mdit_static PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(mdit_static PRIVATE mdit_warnings mdit_sanitizers)
    set_target_properties(mdit_static PROPERTIES OUTPUT_NAME mdit)
endif()

# ---------------------------------------------------------------------------
# CLI / tests / bindings — wired in their own subdirectories when populated.
# ---------------------------------------------------------------------------
if(MDIT_BUILD_CLI AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cli/CMakeLists.txt")
    add_subdirectory(cli)
endif()

if(MDIT_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()

if(MDIT_BUILD_PYBIND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bindings/python/CMakeLists.txt")
    add_subdirectory(bindings/python)
endif()

# ---------------------------------------------------------------------------
# Phase 0 status banner
# ---------------------------------------------------------------------------
message(STATUS "mdit-c ${PROJECT_VERSION} configuring (Phase 0: skeleton)")
