# Doxygen-based API reference for mdit-c.
#
# Gated on ``MDIT_BUILD_DOCS=ON`` and the presence of a Doxygen
# binary. When either is missing, this directory is configured
# silently — a build with ``MDIT_BUILD_DOCS=ON`` but no Doxygen
# installed prints a one-line warning rather than failing.
#
#   cmake -S mdit-c -B build -DMDIT_BUILD_DOCS=ON
#   cmake --build build --target mdit_docs
#   open build/docs/doxygen/html/index.html

find_package(Doxygen QUIET)

if(NOT DOXYGEN_FOUND)
    message(WARNING
        "MDIT_BUILD_DOCS=ON requested but Doxygen was not found; "
        "the `mdit_docs` target will be skipped. Install doxygen "
        "(>=1.9 recommended) to generate the API reference.")
    return()
endif()

# Inputs Doxygen will scan. Order matters for the deterministic file
# listing in the rendered output.
set(_doxy_inputs
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/docs/API.md"
    "${CMAKE_SOURCE_DIR}/docs/PLUGINS.md"
)

# CMake gives us a list; Doxygen wants a single space-separated
# string. Encode the same way it expects in the config file.
string(REPLACE ";" " " _doxy_inputs_str "${_doxy_inputs}")

set(MDIT_PROJECT_VERSION  "${PROJECT_VERSION}")
set(MDIT_DOXY_OUTPUT_DIR  "${CMAKE_BINARY_DIR}/docs/doxygen")
set(MDIT_DOXY_INPUTS      "${_doxy_inputs_str}")
set(MDIT_DOXY_MAINPAGE    "${CMAKE_SOURCE_DIR}/docs/API.md")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in"
    "${CMAKE_BINARY_DIR}/docs/Doxyfile"
    @ONLY
)

add_custom_target(mdit_docs
    COMMAND ${DOXYGEN_EXECUTABLE} "${CMAKE_BINARY_DIR}/docs/Doxyfile"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Generating mdit-c API reference (doxygen)"
    VERBATIM
)
