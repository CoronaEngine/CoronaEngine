# Make CTest (including CLion's All CTest configuration) independent of the
# launching shell's Conan/Python activation on Windows.
include_guard(GLOBAL)

function(_corona_apply_test_runtime directory)
    get_property(_tests DIRECTORY "${directory}" PROPERTY TESTS)
    if(_tests)
        # Append so individual tests keep their own environment settings.
        set_property(TEST ${_tests} DIRECTORY "${directory}" APPEND PROPERTY
            ENVIRONMENT_MODIFICATION ${ARGN})
    endif()

    get_property(_subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(_subdirectory IN LISTS _subdirectories)
        _corona_apply_test_runtime("${_subdirectory}" ${ARGN})
    endforeach()
endfunction()

function(corona_configure_test_runtime directory)
    if(NOT WIN32)
        return()
    endif()

    set(_runtime_files ${CORONA_TBB_REDIS_DLLS})
    if(TARGET Helicon)
        get_target_property(_slang_files Helicon INTERFACE_HELICON_RUNTIME_DEPS)
        if(_slang_files)
            list(APPEND _runtime_files ${_slang_files})
        endif()
    endif()
    if(TARGET CoronaEngine)
        get_target_property(_python_files CoronaEngine INTERFACE_CORONA_RUNTIME_DEPS)
        if(_python_files)
            list(APPEND _runtime_files ${_python_files})
        endif()
    endif()

    set(_runtime_directories)
    foreach(_runtime_file IN LISTS _runtime_files)
        get_filename_component(_runtime_directory "${_runtime_file}" DIRECTORY)
        list(APPEND _runtime_directories "${_runtime_directory}")
    endforeach()
    # These libraries share the renderer runtime directory. Resolve it per
    # configuration rather than assuming a single-config bin directory.
    foreach(_runtime_target IN ITEMS vision-base ocarina-core horizon-hotfix)
        if(TARGET ${_runtime_target})
            list(APPEND _runtime_directories "$<TARGET_FILE_DIR:${_runtime_target}>")
        endif()
    endforeach()
    if(CEF_ROOT)
        list(APPEND _runtime_directories
            "${CEF_ROOT}/$<IF:$<CONFIG:Debug>,Debug,Release>")
    endif()
    list(REMOVE_DUPLICATES _runtime_directories)

    set(_environment)
    foreach(_runtime_directory IN LISTS _runtime_directories)
        list(APPEND _environment "PATH=path_list_prepend:${_runtime_directory}")
    endforeach()
    if(_environment)
        _corona_apply_test_runtime("${directory}" ${_environment})
    endif()
endfunction()
