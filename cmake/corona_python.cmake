# ==============================================================================
# corona_python.cmake
#
# Purpose:
# Provide Python discovery and dependency validation using the conda environment.
#
# Overview:
# 1. Discover the conda environment created by corona_dev_bootstrap.
# 2. Force the build to use the Python from that conda environment.
# 3. Expose configuration knobs:
#    - `CORONA_CONDA_ENV`: conda environment name (default: coronaengine-dev)
# 4. Assert the discovered interpreter is from the conda environment and that
#    its version is 3.13.x to match the ABI (python313.lib).
# 5. Validate requirements listed in `editor/requirements.txt` via
#    `check_pip_modules.py`, optionally installing missing packages. Only runs
#    when `BUILD_CORONA_EDITOR` is enabled.
# 6. Create the `check_python_deps` custom target for manual re-validation.
#
# Design Goals:
# - Use conda-managed Python instead of third_party bundled Python.
# - Provide clear error reporting during configuration.
# - Fail loudly rather than silently using the wrong interpreter.
# - Fail explicitly when dependencies are missing and auto-install is disabled.
# ==============================================================================

include_guard(GLOBAL)

if(NOT DEFINED CORONA_CONDA_ENV OR CORONA_CONDA_ENV STREQUAL "")
    set(CORONA_CONDA_ENV "coronaengine-dev" CACHE STRING "Conda environment name")
endif()

# ------------------------------------------------------------------------------
# Discover Conda Environment Path
# ------------------------------------------------------------------------------
find_program(_corona_conda
    NAMES conda.exe conda.bat conda
    PATHS
        "$ENV{CONDA_PREFIX}/Scripts"
        "$ENV{CONDA_PREFIX}/condabin"
        "$ENV{USERPROFILE}/miniforge3/Scripts"
        "$ENV{USERPROFILE}/miniforge3/condabin"
        "$ENV{USERPROFILE}/miniconda3/Scripts"
        "$ENV{USERPROFILE}/miniconda3/condabin"
        "$ENV{USERPROFILE}/anaconda3/Scripts"
        "$ENV{USERPROFILE}/anaconda3/condabin"
        "$ENV{HOME}/miniconda3/Scripts"
        "$ENV{HOME}/miniconda3/condabin"
        "$ENV{HOME}/anaconda3/Scripts"
        "$ENV{HOME}/anaconda3/condabin"
    DOC "Conda package manager executable"
    REQUIRED
)

# Get the conda environment path
execute_process(
    COMMAND "${_corona_conda}" env list --json
    OUTPUT_VARIABLE _conda_env_list_json
    ERROR_QUIET
    RESULT_VARIABLE _conda_list_result
)

if(NOT _conda_list_result EQUAL 0)
    message(FATAL_ERROR
        "[Python] Failed to list conda environments.\n"
        "  Conda executable: ${_corona_conda}\n"
        "  Make sure conda is properly installed.")
endif()

string(JSON _conda_env_count ERROR_VARIABLE _json_error LENGTH "${_conda_env_list_json}" "envs")
set(_corona_conda_env_path "")

if(NOT _json_error)
    math(EXPR _conda_env_last_index "${_conda_env_count} - 1")
    foreach(_env_index RANGE 0 ${_conda_env_last_index})
        string(JSON _env_path GET "${_conda_env_list_json}" "envs" ${_env_index})
        string(FIND "${_env_path}" "${CORONA_CONDA_ENV}" _env_name_pos)
        if(NOT _env_name_pos EQUAL -1)
            set(_corona_conda_env_path "${_env_path}")
            break()
        endif()
    endforeach()
endif()

if(_corona_conda_env_path STREQUAL "")
    message(FATAL_ERROR
        "[Python] Conda environment '${CORONA_CONDA_ENV}' not found.\n"
        "  Run corona_dev_bootstrap() first to create the environment.")
endif()

message(STATUS "[Python] Using conda environment: ${_corona_conda_env_path}")

# ------------------------------------------------------------------------------
# Python Discovery
# ------------------------------------------------------------------------------
set(Python3_ROOT_DIR "${_corona_conda_env_path}" CACHE FILEPATH "Conda Python3 root directory" FORCE)
set(Python_ROOT_DIR "${_corona_conda_env_path}" CACHE FILEPATH "Conda Python root directory" FORCE)

# Narrow the search to prioritize the conda environment
set(Python_FIND_STRATEGY   LOCATION)  # honour ROOT_DIR location
set(Python_FIND_REGISTRY   NEVER)     # ignore system Python in the Windows registry
set(Python_FIND_VIRTUALENV STANDARD)  # treat conda env as standard virtualenv

find_package(Python COMPONENTS Interpreter Development Development.Module REQUIRED)

# Verify the discovered interpreter is from the conda environment
get_filename_component(_corona_py_real "${Python_EXECUTABLE}" REALPATH)
get_filename_component(_corona_env_real "${_corona_conda_env_path}" REALPATH)
string(TOLOWER "${_corona_py_real}" _corona_py_real_lc)
string(TOLOWER "${_corona_env_real}" _corona_env_real_lc)
string(FIND "${_corona_py_real_lc}" "${_corona_env_real_lc}" _corona_py_pos)

if(NOT _corona_py_pos EQUAL 0)
    message(FATAL_ERROR
        "[Python] 解释器不在 conda 环境内，拒绝继续。\n"
        "  找到: ${Python_EXECUTABLE}\n"
        "  期望位于: ${_corona_conda_env_path}\n"
        "  若在其他 conda/venv 已激活的终端里配置，请退出该环境后重新配置。")
endif()

# Verify Python version is 3.13.x to match the ABI (python313.lib)
if(NOT Python_VERSION MATCHES "^3\\.13\\.")
    message(FATAL_ERROR
        "[Python] 需要 Python 3.13.x 以匹配 python313.lib ABI，实际为 ${Python_VERSION}\n"
        "  Conda 环境 '${CORONA_CONDA_ENV}' 的 Python 版本不正确。\n"
        "  请运行: conda install --name ${CORONA_CONDA_ENV} \"python>=3.13,<3.14\"")
endif()

unset(_corona_py_real)
unset(_corona_env_real)
unset(_corona_py_real_lc)
unset(_corona_env_real_lc)
unset(_corona_py_pos)
unset(_corona_conda_env_path)

message(STATUS "[Python] Final chosen interpreter: ${Python_EXECUTABLE} (${Python_VERSION})")

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
