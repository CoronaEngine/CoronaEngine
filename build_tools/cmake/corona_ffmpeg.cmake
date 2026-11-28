# =============================================================================
# FFmpeg integration
#
# Included by corona_project.cmake before any subdirectory is added, so both the
# resource library (src/resource) and corona_add_example() can use it.
#
# FFmpeg is resolved by Conan. This module gathers the libav* component targets
# behind the corona::ffmpeg aggregate and exposes corona_resource_copy_runtime_dlls()
# for targets that must ship the versioned DLLs next to their executable.
# =============================================================================

include_guard(GLOBAL)

set(_CORONA_FFMPEG_COMPONENTS avutil avcodec avformat swscale swresample)
set(_corona_ffmpeg_targets)
set(_corona_ffmpeg_runtime_dlls)
set(_corona_ffmpeg_missing_runtime_components)
set(CORONA_RESOURCE_HAVE_FFMPEG TRUE CACHE INTERNAL "FFmpeg targets are available")

foreach(_comp IN LISTS _CORONA_FFMPEG_COMPONENTS)
    if(TARGET ffmpeg::${_comp})
        list(APPEND _corona_ffmpeg_targets ffmpeg::${_comp})
    else()
        set(CORONA_RESOURCE_HAVE_FFMPEG FALSE CACHE INTERNAL "FFmpeg targets are available" FORCE)
        message(WARNING "[FFmpeg] Missing Conan target ffmpeg::${_comp}; video/audio FFmpeg support disabled")
    endif()
endforeach()

if(CORONA_RESOURCE_HAVE_FFMPEG)
    add_library(corona_ffmpeg INTERFACE)
    target_link_libraries(corona_ffmpeg INTERFACE ${_corona_ffmpeg_targets})
    add_library(corona::ffmpeg ALIAS corona_ffmpeg)

    # Conan's CMakeDeps generator looks for an unversioned DLL matching each
    # import library (for example avcodec.dll for avcodec.lib). The BtbN
    # package uses ABI-versioned names such as avcodec-62.dll, so those targets
    # are represented as UNKNOWN IMPORTED libraries and are omitted from
    # $<TARGET_RUNTIME_DLLS>. Collect the versioned runtime files explicitly.
    if(WIN32)
        get_cmake_property(_corona_ffmpeg_variables VARIABLES)
        foreach(_corona_ffmpeg_variable IN LISTS _corona_ffmpeg_variables)
            if(_corona_ffmpeg_variable MATCHES "^ffmpeg_BIN_DIRS(_[A-Za-z0-9_]+)?$")
                foreach(_corona_ffmpeg_bin_dir IN LISTS ${_corona_ffmpeg_variable})
                    foreach(_comp IN LISTS _CORONA_FFMPEG_COMPONENTS)
                        file(GLOB _corona_component_runtime_dlls
                            "${_corona_ffmpeg_bin_dir}/${_comp}.dll"
                            "${_corona_ffmpeg_bin_dir}/${_comp}-*.dll")
                        list(APPEND _corona_ffmpeg_runtime_dlls
                            ${_corona_component_runtime_dlls})
                    endforeach()
                endforeach()
            endif()
        endforeach()

        list(REMOVE_DUPLICATES _corona_ffmpeg_runtime_dlls)
        foreach(_comp IN LISTS _CORONA_FFMPEG_COMPONENTS)
            set(_corona_component_runtime_found FALSE)
            foreach(_corona_runtime_dll IN LISTS _corona_ffmpeg_runtime_dlls)
                get_filename_component(_corona_runtime_dll_name
                    "${_corona_runtime_dll}" NAME)
                if(_corona_runtime_dll_name MATCHES "^${_comp}(-.*)?\\.dll$")
                    set(_corona_component_runtime_found TRUE)
                    break()
                endif()
            endforeach()
            if(NOT _corona_component_runtime_found)
                list(APPEND _corona_ffmpeg_missing_runtime_components ${_comp})
            endif()
        endforeach()

        if(_corona_ffmpeg_missing_runtime_components)
            message(FATAL_ERROR
                "[FFmpeg] Missing runtime DLLs in the Conan FFmpeg bin directories: "
                "${_corona_ffmpeg_missing_runtime_components}")
        endif()
    endif()
endif()

set(CORONA_RESOURCE_FFMPEG_RUNTIME_DLLS
    "${_corona_ffmpeg_runtime_dlls}"
    CACHE INTERNAL "FFmpeg runtime DLLs provided by Conan" FORCE)

function(corona_resource_copy_runtime_dlls target_name)
    if(NOT WIN32)
        return()
    endif()
    if(NOT TARGET ${target_name})
        message(FATAL_ERROR
            "corona_resource_copy_runtime_dlls: target '${target_name}' does not exist")
    endif()
    if(NOT CORONA_RESOURCE_FFMPEG_RUNTIME_DLLS)
        message(FATAL_ERROR
            "corona_resource_copy_runtime_dlls: no FFmpeg runtime DLLs were collected")
    endif()
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_RUNTIME_DLLS:${target_name}>"
            ${CORONA_RESOURCE_FFMPEG_RUNTIME_DLLS}
            "$<TARGET_FILE_DIR:${target_name}>"
        COMMAND_EXPAND_LISTS
        VERBATIM)
endfunction()

unset(_CORONA_FFMPEG_COMPONENTS)
unset(_corona_ffmpeg_targets)
unset(_corona_ffmpeg_runtime_dlls)
unset(_corona_ffmpeg_missing_runtime_components)
unset(_corona_ffmpeg_variables)
unset(_corona_ffmpeg_variable)
unset(_corona_ffmpeg_bin_dir)
unset(_corona_component_runtime_dlls)
unset(_corona_component_runtime_found)
unset(_corona_runtime_dll)
unset(_corona_runtime_dll_name)
