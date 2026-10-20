include_guard(GLOBAL)

function(corona_configure_nodejs)
    if(NOT BUILD_CORONA_EDITOR)
        return()
    endif()
    if(NOT Python_EXECUTABLE OR NOT Python_ROOT_DIR OR NOT _corona_conda)
        message(FATAL_ERROR "[Node.js] Configure the project's Conda Python first.")
    endif()

    # Serialize installation across IDE configurations sharing one environment.
    # Release the lock as soon as this function returns.
    file(LOCK "${Python_ROOT_DIR}/conda-meta/corona-nodejs.lock"
        GUARD FUNCTION TIMEOUT 300 RESULT_VARIABLE _node_lock_result)
    if(NOT _node_lock_result STREQUAL "0")
        message(FATAL_ERROR "[Node.js] Cannot lock the Conda environment: ${_node_lock_result}")
    endif()

    set(_node_script "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/build/ensure_nodejs.py")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_node_script}")
    message(STATUS "[Node.js] Checking the editor toolchain in ${Python_ROOT_DIR}")
    execute_process(
        COMMAND "${Python_EXECUTABLE}" "${_node_script}"
                --conda "${_corona_conda}" --prefix "${Python_ROOT_DIR}"
        OUTPUT_VARIABLE _node_json
        RESULT_VARIABLE _node_result
    )
    if(NOT _node_result EQUAL 0)
        message(FATAL_ERROR "[Node.js] Automatic setup failed; see the Conda/checker output above.")
    endif()

    string(JSON _node_dir ERROR_VARIABLE _node_json_error GET "${_node_json}" node_dir)
    if(_node_json_error OR NOT IS_DIRECTORY "${_node_dir}")
        message(FATAL_ERROR "[Node.js] Invalid checker result: ${_node_json}")
    endif()
    # Refresh on every configure, including when switching Conda environments.
    set(CORONA_NODE_DIR "${_node_dir}" CACHE INTERNAL "Conda-managed editor Node.js directory" FORCE)
    message(STATUS "[Node.js] Editor toolchain: ${CORONA_NODE_DIR}")
endfunction()

corona_configure_nodejs()
