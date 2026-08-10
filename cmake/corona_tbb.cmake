include_guard(GLOBAL)

if(NOT TARGET TBB::tbb)
    message(FATAL_ERROR
        "CoronaEngine expects the Conan package target TBB::tbb. "
        "Include corona_third_party before corona_tbb and install dependencies with Conan.")
endif()

set(TBB_FOUND TRUE)
set(TBB_IMPORTED_TARGETS TBB::tbb)
if(NOT DEFINED TBB_VERSION)
    set(TBB_VERSION "provided-by-conan")
endif()

function(_corona_append_existing_dirs _out_var)
    set(_result "${${_out_var}}")

    foreach(_dir IN LISTS ARGN)
        if(_dir STREQUAL "" OR NOT EXISTS "${_dir}")
            continue()
        endif()

        get_filename_component(_dir_abs "${_dir}" ABSOLUTE)
        list(APPEND _result "${_dir_abs}")
    endforeach()

    if(_result)
        list(REMOVE_DUPLICATES _result)
    endif()

    set(${_out_var} "${_result}" PARENT_SCOPE)
endfunction()

function(_corona_collect_tbb_package_roots _out_var)
    set(_roots "")

    foreach(_var IN ITEMS
            TBB_PACKAGE_FOLDER_DEBUG
            TBB_PACKAGE_FOLDER_RELEASE
            TBB_PACKAGE_FOLDER_RELWITHDEBINFO
            TBB_PACKAGE_FOLDER_MINSIZEREL
            TBB_PACKAGE_FOLDER
            onetbb_PACKAGE_FOLDER_DEBUG
            onetbb_PACKAGE_FOLDER_RELEASE
            onetbb_PACKAGE_FOLDER_RELWITHDEBINFO
            onetbb_PACKAGE_FOLDER_MINSIZEREL
            onetbb_PACKAGE_FOLDER)
        if(DEFINED ${_var})
            _corona_append_existing_dirs(_roots "${${_var}}")
        endif()
    endforeach()

    if(DEFINED TBB_DIR AND NOT TBB_DIR STREQUAL "" AND EXISTS "${TBB_DIR}/TBBConfig.cmake")
        get_filename_component(_tbb_root "${TBB_DIR}/../../.." ABSOLUTE)
        _corona_append_existing_dirs(_roots "${_tbb_root}")
    endif()

    set(${_out_var} "${_roots}" PARENT_SCOPE)
endfunction()

function(_corona_collect_files_with_ext _out_var _extension)
    set(_result "")

    foreach(_dir IN LISTS ARGN)
        if(EXISTS "${_dir}")
            file(GLOB _glb CONFIGURE_DEPENDS "${_dir}/*.${_extension}")
            list(APPEND _result ${_glb})
        endif()
    endforeach()

    if(_result)
        list(REMOVE_DUPLICATES _result)
    endif()

    set(${_out_var} "${_result}" PARENT_SCOPE)
endfunction()

function(_corona_collect_tbb_redist_artifacts)
    if(NOT TBB_FOUND)
        return()
    endif()

    if(CMAKE_SIZEOF_VOID_P STREQUAL "8")
        set(_tbb_intel_arch intel64)
        set(_tbb_arch_suffix "")
    else()
        set(_tbb_intel_arch ia32)
        set(_tbb_arch_suffix 32)
    endif()

    set(_tbb_subdir vc14)

    if(DEFINED WINDOWS_STORE AND WINDOWS_STORE)
        set(_tbb_subdir "${_tbb_subdir}_uwp")
    endif()

    set(_candidate_suffixes
        "redist/${_tbb_intel_arch}/${_tbb_subdir}"
        "bin${_tbb_arch_suffix}/${_tbb_subdir}"
        "bin${_tbb_arch_suffix}"
        "bin"
    )

    set(_candidate_dirs "")
    _corona_collect_tbb_package_roots(_tbb_roots)

    foreach(_tbb_root IN LISTS _tbb_roots)
        foreach(_suffix IN LISTS _candidate_suffixes)
            get_filename_component(_dir "${_tbb_root}/${_suffix}" ABSOLUTE)
            list(APPEND _candidate_dirs "${_dir}")
        endforeach()
    endforeach()

    foreach(_var IN ITEMS
            TBB_BINDIRS_DEBUG
            TBB_BINDIRS_RELEASE
            TBB_BINDIRS_RELWITHDEBINFO
            TBB_BINDIRS_MINSIZEREL
            TBB_BINDIRS
            TBB_BIN_DIRS_DEBUG
            TBB_BIN_DIRS_RELEASE
            TBB_BIN_DIRS_RELWITHDEBINFO
            TBB_BIN_DIRS_MINSIZEREL
            TBB_BIN_DIRS
            onetbb_BINDIRS_DEBUG
            onetbb_BINDIRS_RELEASE
            onetbb_BINDIRS_RELWITHDEBINFO
            onetbb_BINDIRS_MINSIZEREL
            onetbb_BINDIRS
            onetbb_BIN_DIRS_DEBUG
            onetbb_BIN_DIRS_RELEASE
            onetbb_BIN_DIRS_RELWITHDEBINFO
            onetbb_BIN_DIRS_MINSIZEREL
            onetbb_BIN_DIRS
            onetbb_TBB_tbb_BIN_DIRS_DEBUG
            onetbb_TBB_tbb_BIN_DIRS_RELEASE
            onetbb_TBB_tbb_BIN_DIRS_RELWITHDEBINFO
            onetbb_TBB_tbb_BIN_DIRS_MINSIZEREL
            onetbb_TBB_tbb_BIN_DIRS
            onetbb_TBB_tbbmalloc_BIN_DIRS_DEBUG
            onetbb_TBB_tbbmalloc_BIN_DIRS_RELEASE
            onetbb_TBB_tbbmalloc_BIN_DIRS_RELWITHDEBINFO
            onetbb_TBB_tbbmalloc_BIN_DIRS_MINSIZEREL
            onetbb_TBB_tbbmalloc_BIN_DIRS
            onetbb_TBB_tbbmalloc_proxy_BIN_DIRS_DEBUG
            onetbb_TBB_tbbmalloc_proxy_BIN_DIRS_RELEASE
            onetbb_TBB_tbbmalloc_proxy_BIN_DIRS_RELWITHDEBINFO
            onetbb_TBB_tbbmalloc_proxy_BIN_DIRS_MINSIZEREL
            onetbb_TBB_tbbmalloc_proxy_BIN_DIRS)
        if(DEFINED ${_var})
            _corona_append_existing_dirs(_candidate_dirs ${${_var}})
        endif()
    endforeach()

    _corona_collect_files_with_ext(CORONA_TBB_REDIS_DLLS dll ${_candidate_dirs})
    _corona_collect_files_with_ext(CORONA_TBB_REDIS_PDBS pdb ${_candidate_dirs})

    set(CORONA_TBB_REDIS_DLLS "${CORONA_TBB_REDIS_DLLS}" CACHE STRING "Collected TBB redist DLLs" FORCE)
    set(CORONA_TBB_REDIS_PDBS "${CORONA_TBB_REDIS_PDBS}" CACHE STRING "Collected TBB redist PDBs" FORCE)
    mark_as_advanced(CORONA_TBB_REDIS_DLLS CORONA_TBB_REDIS_PDBS)

    unset(_tbb_roots)
    unset(_candidate_dirs)
endfunction()

function(corona_copy_tbb_runtime_artifacts target_name)
    if(NOT TARGET ${target_name})
        message(FATAL_ERROR "corona_copy_tbb_runtime_artifacts: target '${target_name}' does not exist")
    endif()

    if(ARGC GREATER 1 AND NOT "${ARGV1}" STREQUAL "")
        set(_destination "${ARGV1}")
    else()
        set(_destination "$<TARGET_FILE_DIR:${target_name}>")
    endif()

    if(NOT CORONA_TBB_REDIS_DLLS AND NOT CORONA_TBB_REDIS_PDBS AND NOT CORONA_TBB_REDIS_DEFS)
        message(WARNING "No TBB runtime artifacts were collected; corona_copy_tbb_runtime_artifacts skipped")
        return()
    endif()

    foreach(_artifact IN LISTS CORONA_TBB_REDIS_DLLS CORONA_TBB_REDIS_PDBS CORONA_TBB_REDIS_DEFS)
        if(NOT EXISTS "${_artifact}")
            message(WARNING "Missing TBB artifact: ${_artifact}")
            continue()
        endif()

        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_destination}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_artifact}" "${_destination}"
            COMMENT "Copying ${_artifact} to runtime directory for ${target_name}"
        )
    endforeach()
endfunction()

if(TBB_FOUND)
    message(STATUS "TBB found: ${TBB_VERSION}")
    message(STATUS "TBB import targets: ${TBB_IMPORTED_TARGETS}")
    _corona_collect_tbb_redist_artifacts()
else()
    message(FATAL_ERROR "TBB not found")
endif()
