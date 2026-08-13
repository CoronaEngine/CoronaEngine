function(corona_dev_bootstrap)
    if(NOT DEFINED CORONA_CONDA_ENV OR CORONA_CONDA_ENV STREQUAL "")
        set(CORONA_CONDA_ENV "coronaengine-dev" CACHE STRING "Conda environment used by CoronaEngine development tools")
    endif()
    if(NOT DEFINED CORONA_DEV_CONFIGURATION OR CORONA_DEV_CONFIGURATION STREQUAL "")
        set(CORONA_DEV_CONFIGURATION "RelWithDebInfo" CACHE STRING "CoronaEngine developer configuration")
    endif()
    if(NOT DEFINED CORONA_DEV_TARGET_FAMILY OR CORONA_DEV_TARGET_FAMILY STREQUAL "")
        set(CORONA_DEV_TARGET_FAMILY "examples" CACHE STRING "CoronaEngine developer target family")
    endif()

    set_property(CACHE CORONA_DEV_CONFIGURATION PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
    set(_corona_configurations Debug Release RelWithDebInfo MinSizeRel)
    list(FIND _corona_configurations "${CORONA_DEV_CONFIGURATION}" _corona_configuration_index)
    if(_corona_configuration_index EQUAL -1)
        message(FATAL_ERROR
            "Unsupported CORONA_DEV_CONFIGURATION='${CORONA_DEV_CONFIGURATION}'. "
            "Expected one of: ${_corona_configurations}")
    endif()

    set_property(CACHE CORONA_DEV_TARGET_FAMILY PROPERTY STRINGS
        core examples tests vision vision-tests vision-oidn)
    set(_corona_target_families core examples tests vision vision-tests vision-oidn)
    list(FIND _corona_target_families "${CORONA_DEV_TARGET_FAMILY}" _corona_target_family_index)
    if(_corona_target_family_index EQUAL -1)
        message(FATAL_ERROR
            "Unsupported CORONA_DEV_TARGET_FAMILY='${CORONA_DEV_TARGET_FAMILY}'. "
            "Expected one of: ${_corona_target_families}")
    endif()

    string(TOLOWER "${CORONA_DEV_CONFIGURATION}" _corona_configuration_slug)
    set(_corona_toolchain
        "${CMAKE_CURRENT_SOURCE_DIR}/build/conan/${CORONA_DEV_TARGET_FAMILY}/${_corona_configuration_slug}/generators/conan_toolchain.cmake")
    set(_corona_build_environment
        "${CMAKE_CURRENT_SOURCE_DIR}/build/conan/${CORONA_DEV_TARGET_FAMILY}/${_corona_configuration_slug}/generators/dev_build_environment.cmake")
    if(NOT DEFINED ENV{CORONA_DEV_BOOTSTRAP_ACTIVE})
        # Search for conda in multiple locations:
        # - conda.exe in Scripts/ (works in Git Bash)
        # - conda.bat in condabin/ (works in cmd/PowerShell)
        # - conda in PATH (Unix-like systems)
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

        # Check if conda environment exists
        # Note: conda create --yes will REMOVE and recreate existing environments, so we must check first
        execute_process(
            COMMAND "${_corona_conda}" env list --json
            OUTPUT_VARIABLE _conda_env_list_json
            ERROR_QUIET
        )

        string(JSON _conda_env_count ERROR_VARIABLE _json_error LENGTH "${_conda_env_list_json}" "envs")
        set(_env_exists FALSE)
        if(NOT _json_error)
            math(EXPR _conda_env_last_index "${_conda_env_count} - 1")
            foreach(_env_index RANGE 0 ${_conda_env_last_index})
                string(JSON _env_path GET "${_conda_env_list_json}" "envs" ${_env_index})
                string(FIND "${_env_path}" "${CORONA_CONDA_ENV}" _env_name_pos)
                if(NOT _env_name_pos EQUAL -1)
                    set(_env_exists TRUE)
                    break()
                endif()
            endforeach()
        endif()

        if(NOT _env_exists)
            message(STATUS "Creating conda environment '${CORONA_CONDA_ENV}' (this may take a few minutes)...")
            execute_process(
                COMMAND "${_corona_conda}" create --yes --name "${CORONA_CONDA_ENV}"
                        --override-channels --channel conda-forge
                        "python>=3.13,<3.14" "conan>=2.28,<3"
                RESULT_VARIABLE _corona_env_create_result
                COMMAND_ECHO STDOUT
            )
            if(NOT _corona_env_create_result EQUAL 0)
                message(FATAL_ERROR
                    "Failed to create conda environment '${CORONA_CONDA_ENV}' (exit code ${_corona_env_create_result}).")
            endif()
        else()
            message(STATUS "Using existing conda environment '${CORONA_CONDA_ENV}'")
        endif()

        message(STATUS "Running CoronaEngine dependency bootstrap...")
        execute_process(
            COMMAND "${_corona_conda}" run --name "${CORONA_CONDA_ENV}" --no-capture-output
                    python tools/dev.py _bootstrap
                    --configuration "${CORONA_DEV_CONFIGURATION}"
                    --target-family "${CORONA_DEV_TARGET_FAMILY}"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            RESULT_VARIABLE _corona_bootstrap_result
            COMMAND_ECHO STDOUT
        )
        if(NOT _corona_bootstrap_result EQUAL 0)
            message(FATAL_ERROR
                "CoronaEngine dependency bootstrap failed (exit code ${_corona_bootstrap_result}).")
        endif()
    endif()
    if(NOT EXISTS "${_corona_toolchain}")
        message(FATAL_ERROR "CoronaEngine Conan toolchain was not generated: ${_corona_toolchain}")
    endif()
    if(NOT EXISTS "${_corona_build_environment}")
        message(FATAL_ERROR "CoronaEngine build environment was not generated: ${_corona_build_environment}")
    endif()
    include("${_corona_build_environment}")
    set(CMAKE_TOOLCHAIN_FILE "${_corona_toolchain}" CACHE FILEPATH "Conan toolchain" FORCE)

    # Conan installs exactly one configuration per build directory, and CMakeDeps
    # guards every usage requirement (include dirs, compile definitions, imported
    # libraries) behind $<$<CONFIG:${CORONA_DEV_CONFIGURATION}>:...>. Any other
    # configuration in this directory therefore compiles with zero third-party
    # include directories and dies on the first external header.
    #
    # Ninja Multi-Config otherwise defaults CMAKE_CONFIGURATION_TYPES to
    # "Debug;Release;RelWithDebInfo" and builds the first entry when no --config
    # is given, which is how IDE-driven builds (VS Code CMake Tools synthesizes a
    # build preset without --config) silently ended up compiling Debug against
    # RelWithDebInfo-only dependencies.
    #
    # Pin the directory to its provisioned configuration so a bare
    # `cmake --build <dir>` is correct and a mismatched --config fails loudly.
    if(CMAKE_GENERATOR MATCHES "Multi-Config|Visual Studio|Xcode")
        set(CMAKE_CONFIGURATION_TYPES "${CORONA_DEV_CONFIGURATION}" CACHE STRING
            "CoronaEngine configurations provisioned by Conan in this build directory" FORCE)
        set(CMAKE_DEFAULT_BUILD_TYPE "${CORONA_DEV_CONFIGURATION}" CACHE STRING
            "Configuration built when --config is omitted" FORCE)
    else()
        set(CMAKE_BUILD_TYPE "${CORONA_DEV_CONFIGURATION}" CACHE STRING
            "CoronaEngine build type provisioned by Conan in this build directory" FORCE)
    endif()
    set(CMAKE_TRY_COMPILE_CONFIGURATION "${CORONA_DEV_CONFIGURATION}" CACHE STRING
        "Configuration used for try_compile checks" FORCE)
endfunction()
