# ============================================================================== 
# corona_editor.cmake
#
# Purpose:
#   Collect and install Corona Editor resources, mirroring the runtime dependency
#   helper pattern.
#
# Overview:
#   1. `corona_configure_corona_editor(<core_target>)`: detect backend/frontend
#      directories and store them on the core target via the
#      `INTERFACE_CORONA_EDITOR_DIRS` property.
#   2. `corona_install_corona_editor(<executable_target>)`: copy those directories
#      next to an executable during the post-build phase.
#
# Design highlights:
#   - Separate collection from installation so executables can opt in as needed.
#   - Keep data on target properties rather than globals for easier extension.
#   - Maintain idempotence: configuring again overwrites the property; only
#     invoking installation on a target adds copy steps.
# ============================================================================== 

include_guard(GLOBAL)

# Both source directories live in the external repository; runtime paths stay unchanged.
set(CORONA_EDITOR_SOURCE_DIR "${PROJECT_SOURCE_DIR}/CoronaEditor")

function(corona_prepare_editor_sources)
    # Different build presets share this checkout. Never update an existing copy.
    file(MAKE_DIRECTORY "${PROJECT_SOURCE_DIR}/.workspace")
    file(LOCK "${PROJECT_SOURCE_DIR}/.workspace/corona-editor.lock"
        GUARD FUNCTION TIMEOUT 600 RESULT_VARIABLE _editor_lock_result)
    if(NOT _editor_lock_result STREQUAL "0")
        message(FATAL_ERROR "[Corona:Editor] Cannot lock source checkout: ${_editor_lock_result}")
    endif()

    if(NOT EXISTS "${CORONA_EDITOR_SOURCE_DIR}")
        find_package(Git REQUIRED)
        message(STATUS "[Corona:Editor] Cloning into ${CORONA_EDITOR_SOURCE_DIR}")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" clone --depth 1 --single-branch
                https://github.com/CoronaEngine/CoronaEditor.git "${CORONA_EDITOR_SOURCE_DIR}"
            RESULT_VARIABLE _editor_clone_result
        )
        if(NOT _editor_clone_result STREQUAL "0")
            message(FATAL_ERROR "[Corona:Editor] Clone failed (${_editor_clone_result}); check Git/network access and re-run CMake.")
        endif()
    endif()

    if(NOT IS_DIRECTORY "${CORONA_EDITOR_SOURCE_DIR}/assets"
       OR NOT EXISTS "${CORONA_EDITOR_SOURCE_DIR}/editor/requirements.txt"
       OR NOT EXISTS "${CORONA_EDITOR_SOURCE_DIR}/editor/Frontend/package.json")
        message(FATAL_ERROR
            "[Corona:Editor] Incomplete checkout at ${CORONA_EDITOR_SOURCE_DIR}. "
            "Expected assets/ and editor/ directly inside CoronaEditor/. "
            "Repair or move this directory aside, then re-run CMake.")
    endif()
    message(STATUS "[Corona:Editor] Source checkout: ${CORONA_EDITOR_SOURCE_DIR}")
endfunction()

# ------------------------------------------------------------------------------
# Function: collect editor resources during configuration
# ------------------------------------------------------------------------------
function(corona_configure_corona_editor target_name)
    if(NOT TARGET ${target_name})
        message(WARNING "[Corona:Editor] Target ${target_name} does not exist; cannot configure editor resources.")
        return()
    endif()

    set(_CORONA_EDITOR_DIR "${CORONA_EDITOR_SOURCE_DIR}/editor")
    set(_CORONA_EXISTING_DIRS)

    if(EXISTS "${_CORONA_EDITOR_DIR}")
        list(APPEND _CORONA_EXISTING_DIRS "${_CORONA_EDITOR_DIR}")
        set_target_properties(${target_name} PROPERTIES INTERFACE_CORONA_EDITOR_DIRS "${_CORONA_EXISTING_DIRS}")
        message(STATUS "[Corona:Editor] Collected CabbageEditor directory for ${target_name}: ${_CORONA_EDITOR_DIR}")
    else()
        message(WARNING "[Corona:Editor] CabbageEditor directory not found: ${_CORONA_EDITOR_DIR}")
    endif()
endfunction()

# ------------------------------------------------------------------------------
# Function: install editor resources to the target directory
# ------------------------------------------------------------------------------
function(corona_install_corona_editor target_name core_target)
    if(NOT TARGET ${target_name})
        message(WARNING "[Corona:Editor] Install target ${target_name} does not exist; skipping editor resource copy.")
        return()
    endif()

    get_target_property(_CORONA_EDITOR_DIRS ${core_target} INTERFACE_CORONA_EDITOR_DIRS)

    if(NOT _CORONA_EDITOR_DIRS)
        message(WARNING "[Corona:Editor] No collected editor directories on ${core_target}; skipping copy for ${target_name}.")
        return()
    endif()

    set(_CORONA_PY_SCRIPT "${PROJECT_SOURCE_DIR}/tools/build/editor_copy_and_build.py")
    if(NOT CORONA_NODE_DIR OR NOT IS_DIRECTORY "${CORONA_NODE_DIR}")
        message(FATAL_ERROR "[Corona:Editor] Conda Node.js is unavailable; re-run CMake configure.")
    endif()
    set(_CORONA_NODE_DIR "${CORONA_NODE_DIR}")

    if(NOT EXISTS "${_CORONA_PY_SCRIPT}")
        message(WARNING "[Corona:Editor] Script not found: ${_CORONA_PY_SCRIPT}; skipping editor copy/build.")
        return()
    endif()

    if(NOT DEFINED Python_EXECUTABLE)
        message(WARNING "[Corona:Editor] Python executable is not available; skipping editor copy/build step.")
        return()
    endif()

    set(_CORONA_SRC_DIR_ARGS)
    foreach(_CORONA_DIR IN LISTS _CORONA_EDITOR_DIRS)
        list(APPEND _CORONA_SRC_DIR_ARGS "--src-dir" "${_CORONA_DIR}")
    endforeach()

    if(WIN32)
        set(_CORONA_WRAPPER_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/corona_editor_install_${target_name}.bat")
        string(CONCAT _SCRIPT_CONTENT
            "@echo off\n"
            "setlocal\n"
            "set DEST_ROOT=%~1\n"
            "set FRONTEND_DIR=%~2\n"
            "echo [Corona:Editor] Installing editor resources to %DEST_ROOT%\n"
            "\"${Python_EXECUTABLE}\" \"${_CORONA_PY_SCRIPT}\" --dest-root \"%DEST_ROOT%\" --merge-content"
        )

        foreach(_CORONA_DIR IN LISTS _CORONA_EDITOR_DIRS)
            string(APPEND _SCRIPT_CONTENT " --src-dir \"${_CORONA_DIR}\"")
        endforeach()

        string(APPEND _SCRIPT_CONTENT
            " --frontend-dir \"%FRONTEND_DIR%\" --node-dir \"${_CORONA_NODE_DIR}\"\n"
            "if errorlevel 1 (\n"
            "    echo [Corona:Editor] Failed to install editor resources\n"
            "    exit /b 1\n"
            ")\n"
            "echo [Corona:Editor] Successfully installed editor resources\n"
        )
        file(WRITE "${_CORONA_WRAPPER_SCRIPT}" "${_SCRIPT_CONTENT}")

        # Custom targets are always considered out-of-date, so the editor
        # copy runs on every build.  Making the executable depend on this
        # target ensures it is triggered even with --target <exe>.
        add_custom_target(corona_editor_install_${target_name}
            COMMAND     cmd /c "${_CORONA_WRAPPER_SCRIPT}" "$<TARGET_FILE_DIR:${target_name}>/CabbageEditor" "$<TARGET_FILE_DIR:${target_name}>/CabbageEditor/Frontend"
            COMMENT     "[Corona:Editor] Installing editor resources..."
            VERBATIM
        )
        add_dependencies(${target_name} corona_editor_install_${target_name})
    else()
        set(_CORONA_WRAPPER_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/corona_editor_install_${target_name}.sh")
        string(CONCAT _SCRIPT_CONTENT
            "#!/bin/bash\n"
            "DEST_ROOT=\"$1\"\n"
            "FRONTEND_DIR=\"$2\"\n"
            "echo \"[Corona:Editor] Installing editor resources to $DEST_ROOT\"\n"
            "\"${Python_EXECUTABLE}\" \"${_CORONA_PY_SCRIPT}\" --dest-root \"$DEST_ROOT\" --merge-content"
        )

        foreach(_CORONA_DIR IN LISTS _CORONA_EDITOR_DIRS)
            string(APPEND _SCRIPT_CONTENT " --src-dir \"${_CORONA_DIR}\"")
        endforeach()

        string(APPEND _SCRIPT_CONTENT
            " --frontend-dir \"$FRONTEND_DIR\" --node-dir \"${_CORONA_NODE_DIR}\"\n"
            "if [ $? -ne 0 ]; then\n"
            "    echo \"[Corona:Editor] Failed to install editor resources\"\n"
            "    exit 1\n"
            "fi\n"
            "echo \"[Corona:Editor] Successfully installed editor resources\"\n"
        )
        file(WRITE "${_CORONA_WRAPPER_SCRIPT}" "${_SCRIPT_CONTENT}")

        add_custom_target(corona_editor_install_${target_name}
            COMMAND     bash "${_CORONA_WRAPPER_SCRIPT}" "$<TARGET_FILE_DIR:${target_name}>/CabbageEditor" "$<TARGET_FILE_DIR:${target_name}>/CabbageEditor/Frontend"
            COMMENT     "[Corona:Editor] Installing editor resources..."
            VERBATIM
        )
        add_dependencies(${target_name} corona_editor_install_${target_name})
    endif()
endfunction()
