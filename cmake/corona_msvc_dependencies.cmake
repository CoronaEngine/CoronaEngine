# Configuration and compilation can run under different Windows code pages.
# Probe the installed language as raw bytes, then normalize compiler dependency
# lines to an ASCII prefix and UTF-8 paths before Ninja consumes them.
if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
    function(corona_detect_msvc_include_prefix)
        set(_dir "${CMAKE_BINARY_DIR}/CMakeFiles/corona-msvc-deps")
        file(MAKE_DIRECTORY "${_dir}")
        file(WRITE "${_dir}/dependency.h" "// Header dependency probe\n")
        file(WRITE "${_dir}/dependency.cpp" "#include \"dependency.h\"\n")
        execute_process(
            COMMAND "${CMAKE_CXX_COMPILER}" /nologo /showIncludes /c
                "${_dir}/dependency.cpp" "/Fo${_dir}/dependency.obj"
            OUTPUT_FILE "${_dir}/output.txt"
            ERROR_FILE "${_dir}/error.txt"
            RESULT_VARIABLE _result
        )
        if(NOT _result EQUAL 0)
            message(FATAL_ERROR "MSVC header dependency probe failed: ${_dir}/error.txt")
        endif()
        file(READ "${_dir}/output.txt" _output)
        string(REPLACE "\n" ";" _lines "${_output}")
        foreach(_line IN LISTS _lines)
            if(NOT _line MATCHES "dependency[.]h\r?$")
                continue()
            endif()
            # Match only ASCII delimiters: the build path may contain characters
            # encoded in the compiler's ANSI code page rather than CMake's UTF-8.
            string(REGEX MATCH "^.*: +" _prefix "${_line}")
            if(NOT _prefix STREQUAL "")
                string(HEX "${_prefix}" _prefix_hex)
                set(CMAKE_CL_SHOWINCLUDES_PREFIX "Note: including file: " PARENT_SCOPE)
                foreach(_lang C CXX)
                    set(CMAKE_${_lang}_CL_SHOWINCLUDES_PREFIX "Note: including file: " PARENT_SCOPE)
                    set(_launcher "${CMAKE_${_lang}_COMPILER_LAUNCHER}")
                    list(PREPEND _launcher "${Python_EXECUTABLE}"
                        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/build/msvc_showincludes.py"
                        "${_prefix_hex}")
                    set(CMAKE_${_lang}_COMPILER_LAUNCHER "${_launcher}" PARENT_SCOPE)
                endforeach()
                return()
            endif()
        endforeach()
        message(FATAL_ERROR "MSVC did not report the probe header: ${_dir}/output.txt")
    endfunction()
    corona_detect_msvc_include_prefix()
endif()
