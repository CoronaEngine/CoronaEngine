# ============================================================================== 
# corona_runtime_deps.cmake
#
# Purpose:
#   Collect and install runtime dependencies (DLLs/PDBs) for executables.
#
# Overview:
#   1. Configure-time (`corona_configure_runtime_deps`): gather Python-related
#      runtime files and store them on the `CoronaEngine` target via the
#      `INTERFACE_CORONA_RUNTIME_DEPS` property.
#   2. Build-time (`corona_install_runtime_deps`): copy the collected files next
#      to any executable target that opts in, preserving the ability to reuse the
#      same dependency list for multiple consumers.
#
# Design highlights:
#   - Decouple collection from installation so the list can be reused.
#   - Store data on target properties instead of globals for easier extension.
#   - Keep the calls idempotent so repeated configuration updates overwrite with
#     the latest data.
# ============================================================================== 

include_guard(GLOBAL)

# ------------------------------------------------------------------------------
# Function: copy one dependency target's runtime file next to an executable
# ------------------------------------------------------------------------------
function(corona_copy_runtime_files source_target target_name)
    if(NOT WIN32)
        return()
    endif()

    if(NOT TARGET ${target_name})
        message(FATAL_ERROR "corona_copy_runtime_files: target '${target_name}' does not exist")
    endif()

    if(NOT TARGET ${source_target})
        message(WARNING "corona_copy_runtime_files: dependency target '${source_target}' does not exist; skipping")
        return()
    endif()

    get_target_property(_corona_runtime_target_type ${source_target} TYPE)

    if(_corona_runtime_target_type STREQUAL "STATIC_LIBRARY"
       OR _corona_runtime_target_type STREQUAL "INTERFACE_LIBRARY"
       OR _corona_runtime_target_type STREQUAL "OBJECT_LIBRARY"
       OR _corona_runtime_target_type STREQUAL "UTILITY")
        return()
    endif()

    add_custom_command(
        TARGET ${target_name}
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:${source_target}>"
            "$<TARGET_FILE_DIR:${target_name}>"
        COMMENT "[Corona:RuntimeDeps] Copy runtime target ${source_target} -> ${target_name}"
        VERBATIM
    )
endfunction()

# ------------------------------------------------------------------------------
# Function: install runtime dependencies to a target directory
# ------------------------------------------------------------------------------
function(corona_install_runtime_deps target_name)
    # Retrieve the collected dependency list stored on the core library
    get_target_property(_CORONA_DEPS CoronaEngine INTERFACE_CORONA_RUNTIME_DEPS)

    if(NOT _CORONA_DEPS)
        message(STATUS "[Corona:RuntimeDeps] No INTERFACE_CORONA_RUNTIME_DEPS; skipping copy")
        return()
    endif()

    set(_CORONA_DESTINATION_DIR "$<TARGET_FILE_DIR:${target_name}>")

    set(_CORONA_PY_COPY "${PROJECT_SOURCE_DIR}/misc/pytools/copy_files.py")

    if(EXISTS "${_CORONA_PY_COPY}" AND DEFINED Python_EXECUTABLE)
        set(_CORONA_DEPS_DIR "${CMAKE_BINARY_DIR}/runtime_deps")
        file(MAKE_DIRECTORY "${_CORONA_DEPS_DIR}")
        string(MD5 _corona_target_hash "${target_name}")
        set(_CORONA_DEPS_LIST "${_CORONA_DEPS_DIR}/${_corona_target_hash}.txt")
        file(WRITE "${_CORONA_DEPS_LIST}" "")

        foreach(_corona_dep_file IN LISTS _CORONA_DEPS)
            file(APPEND "${_CORONA_DEPS_LIST}" "${_corona_dep_file}\n")
        endforeach()

        add_custom_command(
            TARGET      ${target_name}
            POST_BUILD
            COMMAND     "${Python_EXECUTABLE}" "${_CORONA_PY_COPY}" --dest "${_CORONA_DESTINATION_DIR}" --list "${_CORONA_DEPS_LIST}"
            COMMENT     "[Corona:RuntimeDeps] Copy Corona runtime dependencies to target directory -> ${target_name}"
            VERBATIM
        )
    else()
        if(NOT EXISTS "${_CORONA_PY_COPY}")
            message(STATUS "[Corona:RuntimeDeps] Python copy script not found; falling back to copy_if_different")
        else()
            message(STATUS "[Corona:RuntimeDeps] Python not available; falling back to copy_if_different")
        endif()

        add_custom_command(
            TARGET      ${target_name}
            POST_BUILD
            COMMAND     ${CMAKE_COMMAND} -E copy_if_different ${_CORONA_DEPS} "${_CORONA_DESTINATION_DIR}"
            COMMENT     "[Corona:RuntimeDeps] Copy runtime deps (fallback) -> ${target_name}"
            VERBATIM
        )
    endif()

    set(CEF_BIN_DIR "${PROJECT_SOURCE_DIR}/third_party/cef/src/$<IF:$<CONFIG:Debug>,Debug,Release>")
    set(CEF_RES_DIR "${PROJECT_SOURCE_DIR}/third_party/cef/src/Resources")

    # Copy CEF runtime files next to the exe. Debug uses CEF Debug binaries;
    # Release, RelWithDebInfo, and MinSizeRel use the Release runtime.
    add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CEF_BIN_DIR}"
            "${_CORONA_DESTINATION_DIR}"

            # 复制所有资源文件 (locales/, .pak, .dat 等) 到可执行文件目录
            # 注意：CEF 默认期望这些文件就在 exe 旁边，而不是在 Resources 子目录中
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CEF_RES_DIR}"
            "${_CORONA_DESTINATION_DIR}"
    )

endfunction()

# ------------------------------------------------------------------------------
# Function: collect runtime dependencies during configuration
# ------------------------------------------------------------------------------
function(corona_configure_runtime_deps target_name)
    if(NOT TARGET ${target_name})
        message(WARNING "[Corona:RuntimeDeps] Target ${target_name} does not exist; cannot configure runtime dependencies.")
        return()
    endif()

    set(_CORONA_ALL_DEPS)
    if(DEFINED Python_RUNTIME_LIBRARY_DIRS)
        file(GLOB _CORONA_PY_DLLS "${Python_RUNTIME_LIBRARY_DIRS}/*.dll")
        file(GLOB _CORONA_PY_PDBS "${Python_RUNTIME_LIBRARY_DIRS}/*.pdb")
        if(_CORONA_PY_DLLS)
            list(APPEND _CORONA_ALL_DEPS ${_CORONA_PY_DLLS})
        endif()
        if(_CORONA_PY_PDBS)
            list(APPEND _CORONA_ALL_DEPS ${_CORONA_PY_PDBS})
        endif()
    endif()

    if(NOT _CORONA_ALL_DEPS)
        message(WARNING "[Corona:RuntimeDeps] No runtime files collected (Python).")
        return()
    endif()

    list(REMOVE_DUPLICATES _CORONA_ALL_DEPS)
    set_target_properties(${target_name} PROPERTIES INTERFACE_CORONA_RUNTIME_DEPS "${_CORONA_ALL_DEPS}")
    message(STATUS "[Corona:RuntimeDeps] Collected ${target_name} files: ${_CORONA_ALL_DEPS}")
endfunction()
