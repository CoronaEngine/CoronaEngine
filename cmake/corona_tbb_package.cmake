include_guard(GLOBAL)

function(corona_tbb_package_has_runtime tbb_config_dir out_var)
    set(_ok FALSE)

    if(EXISTS "${tbb_config_dir}/TBBConfig.cmake")
        if(WIN32)
            get_filename_component(_tbb_root "${tbb_config_dir}/../../.." ABSOLUTE)

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

            set(_lib_suffixes
                "lib/${_tbb_intel_arch}/${_tbb_subdir}"
                "lib${_tbb_arch_suffix}/${_tbb_subdir}"
                "lib${_tbb_arch_suffix}"
                "lib"
            )
            set(_dll_suffixes
                "redist/${_tbb_intel_arch}/${_tbb_subdir}"
                "bin${_tbb_arch_suffix}/${_tbb_subdir}"
                "bin${_tbb_arch_suffix}"
                "bin"
            )
            set(_components tbb12 tbbmalloc tbbmalloc_proxy)
            set(_ok TRUE)

            foreach(_component IN LISTS _components)
                set(_lib_found FALSE)
                foreach(_suffix IN LISTS _lib_suffixes)
                    if(EXISTS "${_tbb_root}/${_suffix}/${_component}.lib")
                        set(_lib_found TRUE)
                        break()
                    endif()
                endforeach()

                set(_dll_found FALSE)
                foreach(_suffix IN LISTS _dll_suffixes)
                    if(EXISTS "${_tbb_root}/${_suffix}/${_component}.dll")
                        set(_dll_found TRUE)
                        break()
                    endif()
                endforeach()

                if(NOT _lib_found OR NOT _dll_found)
                    set(_ok FALSE)
                    break()
                endif()
            endforeach()
        else()
            set(_ok TRUE)
        endif()
    endif()

    set(${out_var} "${_ok}" PARENT_SCOPE)
endfunction()

function(corona_select_valid_tbb_dir out_var)
    set(_selected "")

    foreach(_candidate IN LISTS ARGN)
        if(_candidate STREQUAL "")
            continue()
        endif()

        get_filename_component(_candidate_abs "${_candidate}" ABSOLUTE)
        corona_tbb_package_has_runtime("${_candidate_abs}" _candidate_ok)
        if(_candidate_ok)
            set(_selected "${_candidate_abs}")
            break()
        endif()
    endforeach()

    set(${out_var} "${_selected}" PARENT_SCOPE)
endfunction()
