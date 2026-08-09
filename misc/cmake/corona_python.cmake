# ==============================================================================
# corona_python.cmake
#
# Purpose:
# Provide embedded Python discovery and dependency validation.
#
# Overview:
# 1. Force the build to use the bundled Python toolchain located under
# `third_party/Python-3.13.7`.
# 2. Expose configuration knobs:
# - `CORONA_EMBEDDED_PY_DIR`: bundled Python root (overridable on the
# command line; `Python_ROOT_DIR` is re-forced from it every configure).
# 3. Assert the discovered interpreter really is the bundled one, and that its
# ABI matches `libs/python313.lib`. Without these the wrong interpreter is
# silently baked into the binaries via the `CORONA_PYTHON_*` macros
# (see corona_compile_config.cmake) instead of failing at configure time.
# 4. Validate requirements listed in `editor/requirements.txt` via
# `check_pip_modules.py`, optionally installing missing packages. Only runs
# when `BUILD_CORONA_EDITOR` is enabled.
# 5. Create the `check_python_deps` custom target for manual re-validation.
#
# Design Goals:
# - Provide clear error reporting during configuration.
# - Fail loudly rather than silently using a non-bundled interpreter.
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

# Narrow the search before FindPython runs. corona_dev_bootstrap() already
# injected the conda dev environment into ENV{PATH}, and that environment ships
# its own (unpinned, `python>=3.11`) interpreter. Without these, configuring
# from an activated conda/venv shell lets CONDA_PREFIX/VIRTUAL_ENV win over the
# ROOT_DIR hint.
set(Python_FIND_STRATEGY   LOCATION)  # honour ROOT_DIR location, do not prefer newest version
set(Python_FIND_REGISTRY   NEVER)     # ignore system Python in the Windows registry
set(Python_FIND_VIRTUALENV STANDARD)  # do not let CONDA_PREFIX/VIRTUAL_ENV take precedence

find_package(Python COMPONENTS Interpreter Development Development.Module REQUIRED)

# `REQUIRED` above already fails when nothing is found, so testing Python_FOUND
# here would be dead code. The real risk is finding the *wrong* interpreter:
# corona_compile_config.cmake bakes these paths into every target as
# CORONA_PYTHON_* macros, and python_api.cpp feeds them to PyConfig at runtime.
# A mismatch there is silent at configure time and fails at link or run time.
get_filename_component(_corona_py_real "${Python_EXECUTABLE}"      REALPATH)
get_filename_component(_corona_py_root "${CORONA_EMBEDDED_PY_DIR}" REALPATH)
string(TOLOWER "${_corona_py_real}" _corona_py_real_lc)
string(TOLOWER "${_corona_py_root}" _corona_py_root_lc)
string(FIND "${_corona_py_real_lc}" "${_corona_py_root_lc}" _corona_py_pos)

if(NOT _corona_py_pos EQUAL 0)
    message(FATAL_ERROR
        "[Python] 解释器不在嵌入目录内，拒绝继续。\n"
        "  找到: ${Python_EXECUTABLE}\n"
        "  期望位于: ${CORONA_EMBEDDED_PY_DIR}\n"
        "  编译期会把该路径与 ABI 烘焙进二进制（CORONA_PYTHON_* 宏），"
        "错误的解释器将导致链接或运行时失败。\n"
        "  若在 conda/venv 已激活的终端里配置，请退出该环境后重新配置。")
endif()

# Python::Python links libs/python313.lib, so the interpreter's major.minor must
# match. Checked on major.minor rather than EXACT so a patch-level refresh of
# the bundled toolchain does not need a code change; the ABI is set by 3.13.
if(NOT Python_VERSION MATCHES "^3\\.13\\.")
    message(FATAL_ERROR
        "[Python] 需要 3.13.x 以匹配 ${CORONA_EMBEDDED_PY_DIR}/libs/python313.lib，实际为 ${Python_VERSION}")
endif()

unset(_corona_py_real)
unset(_corona_py_root)
unset(_corona_py_real_lc)
unset(_corona_py_root_lc)
unset(_corona_py_pos)

message(STATUS "[Python] Final chosen interpreter: ${Python_EXECUTABLE} (${Python_VERSION})")

set(CORONA_PY_REQUIREMENTS_FILE "${PROJECT_SOURCE_DIR}/editor/requirements.txt")
set(CORONA_PY_CHECK_SCRIPT "${PROJECT_SOURCE_DIR}/misc/pytools/check_pip_modules.py")

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

