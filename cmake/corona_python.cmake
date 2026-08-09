# ==============================================================================
# corona_python.cmake
#
# Purpose:
# Provide embedded Python discovery and dependency validation.
#
# Overview:
# 1. Force the build to use the bundled Python toolchain (minimum version
# enforced) located under `third_party/Python-3.13.7`.
# 2. Expose configuration knobs:
# - `CORONA_PYTHON_MIN_VERSION`: expected minimum Python version
# (major.minor).
# 3. Validate requirements listed in `editor/requirements.txt` via
# `check_pip_modules.py`, optionally installing missing packages.
# 4. Create the `check_python_deps` custom target for manual re-validation.
#
# Design Goals:
# - Provide clear error reporting during configuration.
# - Fail explicitly when dependencies are missing and auto-install is disabled.
# ==============================================================================

include_guard(GLOBAL)

# set(CORONA_PYTHON_MIN_VERSION 3.13 CACHE STRING "Minimum required Python version (major.minor)")
set(CORONA_EMBEDDED_PY_DIR "${PROJECT_SOURCE_DIR}/third_party/Python-3.13.7" CACHE PATH "Embedded (bundled) Python directory path")

# ------------------------------------------------------------------------------
# Python Discovery
# ------------------------------------------------------------------------------
set(Python3_ROOT_DIR "${CORONA_EMBEDDED_PY_DIR}" CACHE FILEPATH "Embedded Python3 root directory" FORCE)
set(Python_ROOT_DIR "${CORONA_EMBEDDED_PY_DIR}" CACHE FILEPATH "Embedded Python root directory" FORCE)
message(STATUS "[Python3] Using embedded Python3: ${Python3_ROOT_DIR}")
message(STATUS "[Python] Using embedded Python: ${Python_ROOT_DIR}")

find_package(Python COMPONENTS Interpreter Development Development.Module REQUIRED)

if(NOT Python_FOUND)
    message(FATAL_ERROR "[Python] Embedded Python interpreter not found at ${CORONA_EMBEDDED_PY_DIR}; cannot continue")
endif()

message(STATUS "[Python] Final chosen interpreter: ${Python_EXECUTABLE}")

set(CORONA_PY_REQUIREMENTS_FILE "${PROJECT_SOURCE_DIR}/editor/requirements.txt")
set(CORONA_PY_CHECK_SCRIPT "${PROJECT_SOURCE_DIR}/tools/build/check_pip_modules.py")

# ------------------------------------------------------------------------------
# Helper: run a Python script
# ------------------------------------------------------------------------------
function(corona_run_python OUT_RESULT)
    set(options)
    set(oneValueArgs SCRIPT WORKING_DIRECTORY)
    set(multiValueArgs ARGS)
    cmake_parse_arguments(CRP "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT CRP_SCRIPT)
        message(FATAL_ERROR "corona_run_python: SCRIPT is required")
    endif()

    if(NOT DEFINED Python_EXECUTABLE)
        message(FATAL_ERROR "corona_run_python: Python_EXECUTABLE is not defined")
    endif()

    if(NOT CRP_WORKING_DIRECTORY)
        set(CRP_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    endif()

    execute_process(
        COMMAND "${Python_EXECUTABLE}" "${CRP_SCRIPT}" ${CRP_ARGS}
        WORKING_DIRECTORY "${CRP_WORKING_DIRECTORY}"
        RESULT_VARIABLE _CRP_RES
        OUTPUT_VARIABLE _CRP_OUT
        ERROR_VARIABLE _CRP_ERR
    )
    set(${OUT_RESULT} ${_CRP_RES} PARENT_SCOPE)
    set(CORONA_LAST_PY_STDOUT "${_CRP_OUT}" PARENT_SCOPE)
    set(CORONA_LAST_PY_STDERR "${_CRP_ERR}" PARENT_SCOPE)
endfunction()

# ------------------------------------------------------------------------------
# Helper: validate Python requirements
# ------------------------------------------------------------------------------
function(corona_run_python_requirements_check)
    if(NOT EXISTS "${CORONA_PY_CHECK_SCRIPT}")
        message(WARNING "[Python] Dependency check script missing: ${CORONA_PY_CHECK_SCRIPT}")
        return()
    endif()

    if(NOT EXISTS "${CORONA_PY_REQUIREMENTS_FILE}")
        message(WARNING "[Python] requirements.txt not found: ${CORONA_PY_REQUIREMENTS_FILE}")
        return()
    endif()

    set(_CRP_ARGS -r "${CORONA_PY_REQUIREMENTS_FILE}" --no-unicode)

    if(CORONA_AUTO_INSTALL_PY_DEPS)
        list(APPEND _CRP_ARGS --auto-install)
    endif()

    message(STATUS "[Python] Running dependency check with interpreter: ${Python_EXECUTABLE}")
    corona_run_python(_CORONA_PY_RES
        SCRIPT "${CORONA_PY_CHECK_SCRIPT}"
        ARGS ${_CRP_ARGS}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    if(NOT _CORONA_PY_RES EQUAL 0)
        if(CORONA_LAST_PY_STDOUT)
            message(STATUS "[Python] Checker stdout:\n${CORONA_LAST_PY_STDOUT}")
        endif()

        if(CORONA_LAST_PY_STDERR)
            message(STATUS "[Python] Checker stderr:\n${CORONA_LAST_PY_STDERR}")
        endif()

        message(FATAL_ERROR "[Python] Requirement check failed (exit code ${_CORONA_PY_RES}); see output above, fix issues, then re-run")
    else()
        message(STATUS "[Python] Requirements satisfied; no installation needed")
    endif()
endfunction()

if(BUILD_CORONA_EDITOR)
    corona_run_python_requirements_check()
    add_custom_target(check_python_deps
            COMMAND "${Python_EXECUTABLE}" "${CORONA_PY_CHECK_SCRIPT}" -r "${CORONA_PY_REQUIREMENTS_FILE}" --no-unicode
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            COMMENT "[Python] Manually trigger dependency check"
            VERBATIM
    )
endif()

