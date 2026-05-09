# run_smoke.cmake — drives the install-consumer smoke test from CTest.
#
# Inputs (set via -D on the command line):
#   STAGE_DIR         where to stage the install (will be cleaned + reused)
#   SOURCE_DIR        path to the smoke project sources
#   BINARY_DIR        scratch build directory for the smoke project
#   SUPER_BUILD_DIR   the parent project's build directory (the one that
#                     produced the static library being installed)
#   CONFIG            build configuration (Debug / RelWithDebInfo / …)
#   GENERATOR         CMake generator name to mirror the parent build
#                     (`Visual Studio 17 2022`, `Ninja`, …)
#
# We perform `install -> configure -> build -> run` in sequence, failing
# loudly the first step that returns non-zero. Everything happens in
# `STAGE_DIR` so repeated runs are deterministic.

if(NOT STAGE_DIR OR NOT SOURCE_DIR OR NOT BINARY_DIR OR NOT SUPER_BUILD_DIR)
    message(FATAL_ERROR
        "run_smoke.cmake: STAGE_DIR / SOURCE_DIR / BINARY_DIR / "
        "SUPER_BUILD_DIR all required.")
endif()
if(NOT CONFIG)
    set(CONFIG "RelWithDebInfo")
endif()

# Clean prior staging so a removed file doesn't linger.
file(REMOVE_RECURSE "${STAGE_DIR}")
file(MAKE_DIRECTORY "${STAGE_DIR}")
file(REMOVE_RECURSE "${BINARY_DIR}")
file(MAKE_DIRECTORY "${BINARY_DIR}")

# Step 1: install the parent project into STAGE_DIR.
execute_process(
    COMMAND ${CMAKE_COMMAND} --install "${SUPER_BUILD_DIR}"
            --config "${CONFIG}"
            --prefix "${STAGE_DIR}"
    RESULT_VARIABLE _rc
)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "install_smoke: --install failed (${_rc})")
endif()

# Step 2: configure the smoke consumer against the staged package.
set(_configure ${CMAKE_COMMAND}
        -S "${SOURCE_DIR}"
        -B "${BINARY_DIR}"
        -DCMAKE_PREFIX_PATH=${STAGE_DIR}
        -DCMAKE_BUILD_TYPE=${CONFIG}
)
if(GENERATOR)
    list(APPEND _configure -G "${GENERATOR}")
endif()
execute_process(COMMAND ${_configure} RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "install_smoke: smoke configure failed (${_rc})")
endif()

# Step 3: build the smoke executable.
execute_process(
    COMMAND ${CMAKE_COMMAND} --build "${BINARY_DIR}" --config "${CONFIG}"
    RESULT_VARIABLE _rc
)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "install_smoke: smoke build failed (${_rc})")
endif()

# Step 4: run the smoke executable. Its only job is to render `# hi\n`
# and exit 0; non-zero is a failure.
file(GLOB _smoke_exe
    "${BINARY_DIR}/smoke"
    "${BINARY_DIR}/smoke.exe"
    "${BINARY_DIR}/${CONFIG}/smoke.exe"
)
list(LENGTH _smoke_exe _n)
if(_n EQUAL 0)
    message(FATAL_ERROR
        "install_smoke: built smoke executable not found under ${BINARY_DIR}")
endif()
list(GET _smoke_exe 0 _exe)
get_filename_component(_exe_dir "${_exe}" DIRECTORY)

# When the parent build produced a shared library, the smoke binary
# needs the DLL / shared object on its loader search path.
#
# * Windows: copy the staged `*.dll` next to `smoke.exe` (sidesteps
#   PATH manipulation, which `cmake -E env` mangles because Windows
#   PATH uses `;` and CMake interprets that as a list separator).
# * POSIX: prepend the staged libdir to LD_LIBRARY_PATH /
#   DYLD_LIBRARY_PATH (colon-separated, so no CMake list collisions).
if(WIN32)
    file(GLOB _stage_dlls "${STAGE_DIR}/bin/*.dll")
    foreach(_dll IN LISTS _stage_dlls)
        file(COPY "${_dll}" DESTINATION "${_exe_dir}")
    endforeach()
    execute_process(COMMAND "${_exe}" RESULT_VARIABLE _rc)
else()
    if(APPLE)
        set(_libpath_var DYLD_LIBRARY_PATH)
    else()
        set(_libpath_var LD_LIBRARY_PATH)
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            "${_libpath_var}=${STAGE_DIR}/lib:$ENV{${_libpath_var}}"
            "${_exe}"
        RESULT_VARIABLE _rc
    )
endif()
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "install_smoke: smoke ran but failed (${_rc})")
endif()
message(STATUS "install_smoke: PASS")
