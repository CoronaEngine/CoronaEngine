# ==============================================================================
# CoronaEngine - Examples Build Configuration
#
# Responsibilities:
# - Provide per-example build toggles (`BUILD_CORONA_EXAMPLE_<NAME>`).
# - Define `corona_add_example()` for consistent example setup.
# - Discover and add example subdirectories automatically.
# - Assign the first enabled example as the Visual Studio startup project.
# ==============================================================================

include(CMakeParseArguments)

# ------------------------------------------------------------------------------
# Global properties used to track example targets
# ------------------------------------------------------------------------------
define_property(GLOBAL PROPERTY _CORONA_EXAMPLE_TARGETS
    BRIEF_DOCS "Corona example targets"
    FULL_DOCS "List of Corona example targets created via corona_add_example"
)
define_property(GLOBAL PROPERTY _CORONA_EXAMPLE_REGISTRY
    BRIEF_DOCS "Corona example registry"
    FULL_DOCS "Pairs of <folder>|<target> captured when corona_add_example is invoked"
)
set_property(GLOBAL PROPERTY _CORONA_EXAMPLE_TARGETS "")
set_property(GLOBAL PROPERTY _CORONA_EXAMPLE_REGISTRY "")

# ------------------------------------------------------------------------------
# Helper: copy assets into an example output directory
# ------------------------------------------------------------------------------
function(corona_examples_copy_assets target_name)
    add_custom_command(
        TARGET ${target_name}
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "[${target_name}] Copy assets to $<TARGET_FILE_DIR:${target_name}>/assets"
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${target_name}>/assets
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/assets" $<TARGET_FILE_DIR:${target_name}>/assets
        VERBATIM
    )

    if(CORONA_BUILD_VISION AND TARGET ocarina)
        add_custom_command(
            TARGET ${target_name}
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E echo "[${target_name}] Copy vision runtime deps from $<TARGET_FILE_DIR:ocarina>/. to $<TARGET_FILE_DIR:${target_name}>/"
            COMMAND ${CMAKE_COMMAND} -E copy_directory $<TARGET_FILE_DIR:ocarina>/. $<TARGET_FILE_DIR:${target_name}>/.
            VERBATIM
        )
    elseif(CORONA_BUILD_VISION)
        message(STATUS "[${target_name}] Skip Ocarina runtime deps copy because target 'ocarina' is not available")
    endif()

    # The CUDA backend links against NVRTC, which is not part of the Ocarina
    # target output directory. Stage the NVRTC DLL and its companion runtime
    # DLLs next to the example so LoadLibrary does not depend on the user's
    # global CUDA PATH.
    if(CORONA_BUILD_VISION AND WIN32 AND DEFINED CUDAToolkit_BIN_DIR)
        file(GLOB _corona_cuda_runtime_files
            "${CUDAToolkit_BIN_DIR}/x64/nvrtc*.dll"
            "${CUDAToolkit_BIN_DIR}/nvrtc*.dll"
            "${CUDAToolkit_BIN_DIR}/x64/nvJitLink*.dll"
            "${CUDAToolkit_BIN_DIR}/nvJitLink*.dll")
        list(REMOVE_DUPLICATES _corona_cuda_runtime_files)

        foreach(_corona_cuda_runtime_file IN LISTS _corona_cuda_runtime_files)
            add_custom_command(
                TARGET ${target_name}
                POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_corona_cuda_runtime_file}"
                    "$<TARGET_FILE_DIR:${target_name}>"
                VERBATIM
            )
        endforeach()
    endif()
endfunction()

# ------------------------------------------------------------------------------
# Helper: define an example executable
#
# Arguments:
# NAME <name>           - example target name (required)
# SOURCES <files...>    - source file list (required)
# COPY_ASSETS           - optional flag to copy the shared assets directory
#
# Behavior:
# - Create an executable target linked against CoronaEngine and dependencies.
# - Install runtime dependencies (Helicon/Python/editor) when available.
# - Optionally copy shared assets next to the built executable.
# ------------------------------------------------------------------------------
function(corona_add_example)
    set(options COPY_ASSETS)
    set(oneValueArgs NAME)
    set(multiValueArgs SOURCES)
    cmake_parse_arguments(CORONA_EX "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT CORONA_EX_NAME)
        message(FATAL_ERROR "corona_add_example requires NAME")
    endif()

    if(NOT CORONA_EX_SOURCES)
        message(FATAL_ERROR "corona_add_example requires SOURCES for ${CORONA_EX_NAME}")
    endif()

    add_executable(${CORONA_EX_NAME} ${CORONA_EX_SOURCES})

    target_compile_features(${CORONA_EX_NAME} PRIVATE cxx_std_20)
    
    # Analyze the generator to set the working directory correctly
    # VS Generator sometimes fails to expand $<TARGET_FILE_DIR> in this property, so we use the MSBuild macro.
    set_target_properties(${CORONA_EX_NAME} PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:${CORONA_EX_NAME}>)


    # target_link_libraries(${CORONA_EX_NAME} PRIVATE CoronaEngine cabbage::concurrent)
    if(COMMAND helicon_install_runtime_deps)
        helicon_install_runtime_deps(${CORONA_EX_NAME})
    else()
        message(STATUS "[Corona:Example:${CORONA_EX_NAME}] helicon_install_runtime_deps not available; skipping Helicon runtime dependencies")
    endif()

    corona_install_runtime_deps(${CORONA_EX_NAME})
    corona_copy_tbb_runtime_artifacts(${CORONA_EX_NAME})
    corona_copy_runtime_files(assimp::assimp ${CORONA_EX_NAME})

    # Copy FFmpeg runtime DLLs (video/audio support in corona_resource).
    # These are IMPORTED SHARED targets reached through a PRIVATE link chain,
    # which corona_copy_runtime_files cannot resolve; $<TARGET_RUNTIME_DLLS>
    # (used by corona_resource_copy_runtime_dlls) handles them.
    if(CORONA_RESOURCE_HAVE_FFMPEG AND COMMAND corona_resource_copy_runtime_dlls)
        corona_resource_copy_runtime_dlls(${CORONA_EX_NAME})
    endif()

    if(BUILD_CORONA_EDITOR)
        corona_install_corona_editor(${CORONA_EX_NAME} CoronaEngine)
    endif()

    if(CORONA_EX_COPY_ASSETS)
        corona_examples_copy_assets(${CORONA_EX_NAME})
    endif()

    set_property(GLOBAL APPEND PROPERTY _CORONA_EXAMPLE_TARGETS ${CORONA_EX_NAME})

    if(DEFINED CORONA_CURRENT_EXAMPLE_FOLDER)
        set_property(GLOBAL APPEND PROPERTY _CORONA_EXAMPLE_REGISTRY
            "${CORONA_CURRENT_EXAMPLE_FOLDER}|${CORONA_EX_NAME}"
        )
    endif()

    message(STATUS "[Corona:Example] ${CORONA_EX_NAME} configured")
endfunction()

# ------------------------------------------------------------------------------
# Discover and add example subdirectories
# ------------------------------------------------------------------------------
file(GLOB _corona_example_dirs RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "${CMAKE_CURRENT_SOURCE_DIR}/*")
list(SORT _corona_example_dirs)
set(_corona_enabled_examples)

foreach(_corona_folder IN LISTS _corona_example_dirs)
    if(NOT IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${_corona_folder}")
        continue()
    endif()

    if(_corona_folder STREQUAL "assets")
        continue()
    endif()

    string(TOUPPER "${_corona_folder}" _corona_upper)
    string(REPLACE "/" "_" _corona_upper "${_corona_upper}")
    string(REPLACE "-" "_" _corona_upper "${_corona_upper}")
    set(_corona_option_name "BUILD_CORONA_EXAMPLE_${_corona_upper}")
    option(${_corona_option_name} "Build ${_corona_folder} example" ON)

    if(${_corona_option_name})
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_corona_folder}/CMakeLists.txt")
            set(CORONA_CURRENT_EXAMPLE_FOLDER "${_corona_folder}")
            add_subdirectory(${_corona_folder})
            list(APPEND _corona_enabled_examples "${_corona_folder}")
        else()
            message(WARNING "[Corona:Examples] ${_corona_folder} subdir not found, skip")
        endif()
    endif()
endforeach()

unset(CORONA_CURRENT_EXAMPLE_FOLDER)

get_property(_corona_example_targets GLOBAL PROPERTY _CORONA_EXAMPLE_TARGETS)

if(_corona_example_targets)
    list(FILTER _corona_example_targets EXCLUDE REGEX "^$")
    list(REMOVE_DUPLICATES _corona_example_targets)
    list(LENGTH _corona_example_targets _corona_target_count)

    if(_corona_target_count GREATER 0)
        list(GET _corona_example_targets 0 _corona_startup_target)
        set_property(DIRECTORY ${PROJECT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ${_corona_startup_target})
    endif()
endif()

# ------------------------------------------------------------------------------
# Output configuration summary
# ------------------------------------------------------------------------------
message(STATUS "")
message(STATUS "================================================================================")
message(STATUS "  Corona Examples Summary")
message(STATUS "================================================================================")

if(_corona_enabled_examples)
    foreach(_corona_folder IN LISTS _corona_enabled_examples)
        message(STATUS "  ✓ ${_corona_folder}")
    endforeach()
else()
    message(STATUS "  (no examples enabled)")
endif()

get_property(_corona_example_registry GLOBAL PROPERTY _CORONA_EXAMPLE_REGISTRY)

if(_corona_example_registry)
    list(FILTER _corona_example_registry EXCLUDE REGEX "^$")
    list(REMOVE_DUPLICATES _corona_example_registry)
    set(_corona_pairs)

    foreach(_corona_entry IN LISTS _corona_example_registry)
        string(REPLACE "|" ";" _corona_split "${_corona_entry}")
        list(LENGTH _corona_split _corona_split_len)

        if(_corona_split_len EQUAL 2)
            list(GET _corona_split 0 _corona_folder_name)
            list(GET _corona_split 1 _corona_target_name)
            list(APPEND _corona_pairs "${_corona_folder_name}->${_corona_target_name}")
        else()
            list(APPEND _corona_pairs "${_corona_entry}")
        endif()
    endforeach()

    string(JOIN ", " _corona_pairs_joined ${_corona_pairs})
    message(STATUS "  Linked targets: ${_corona_pairs_joined}")
else()
    message(STATUS "  Linked targets: (none)")
endif()

message(STATUS "================================================================================")
message(STATUS "")
