# ==============================================================================
# corona_compile_config.cmake
#
# Purpose:
# Centralize compiler settings and shared compile-time definitions.
#
# Highlights:
# - Maintain consistent definitions across modules.
# - Reduce scattering of preprocessor macros and toolchain tweaks.
# - Employ generator expressions (`$<...>`) for compiler/config-specific
# adjustments.
# ==============================================================================

include_guard(GLOBAL)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ------------------------------------------------------------------------------
# Helper: convert path separators to backslashes
#
# `corona_to_backslash(INPUT out_var [ESCAPE_FOR_CSTRING])`
#
# Replaces forward slashes with backslashes. When `ESCAPE_FOR_CSTRING` is
# provided, the result is escaped for use inside C string literals (each
# backslash becomes `\\`).
# ------------------------------------------------------------------------------
function(corona_to_backslash INPUT OUT_VAR)
    set(_val "${INPUT}")

    string(REPLACE "/" "\\" _val "${_val}")

    if(ARGC GREATER 2 AND ARGV2 STREQUAL "ESCAPE_FOR_CSTRING")
        string(REPLACE "\\" "\\\\" _val "${_val}")
    endif()

    set(${OUT_VAR} "${_val}" PARENT_SCOPE)
endfunction()

# ------------------------------------------------------------------------------
# Python path macro definitions
# ------------------------------------------------------------------------------
corona_to_backslash("${Python_EXECUTABLE}" _CORONA_PY_EXE_ESC ESCAPE_FOR_CSTRING)
corona_to_backslash("${Python_RUNTIME_LIBRARY_DIRS}" _CORONA_PY_HOME_ESC ESCAPE_FOR_CSTRING)
corona_to_backslash("${Python_RUNTIME_LIBRARY_DIRS}/DLLs" _CORONA_PY_DLLS_ESC ESCAPE_FOR_CSTRING)
corona_to_backslash("${Python_RUNTIME_LIBRARY_DIRS}/Lib" _CORONA_PY_LIB_ESC ESCAPE_FOR_CSTRING)

# ------------------------------------------------------------------------------
# Global compile definitions
# ------------------------------------------------------------------------------
add_compile_definitions(
    $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>

    NOMINMAX

    ENTT_ID_TYPE=uint64_t

    ENTT_USE_ATOMIC

    CORONA_PYTHON_EXE=\"${_CORONA_PY_EXE_ESC}\"
    CORONA_PYTHON_HOME_DIR=\"${_CORONA_PY_HOME_ESC}\"
    CORONA_PYTHON_MODULE_DLL_DIR=\"${_CORONA_PY_DLLS_ESC}\"
    CORONA_PYTHON_MODULE_LIB_DIR=\"${_CORONA_PY_LIB_ESC}\"
)

if(CORONA_BUILD_HARDWARE)
    add_compile_definitions(CORONA_ENABLE_HARDWARE)
endif()

if(CORONA_BUILD_VISION)
    add_compile_definitions(CORONA_ENABLE_VISION)
endif()

# Enable Python API macros only when building the editor
if(BUILD_CORONA_EDITOR)
    add_compile_definitions(CORONA_ENABLE_PYTHON_API)
endif()

# ------------------------------------------------------------------------------
# MSVC runtime strategy
# ------------------------------------------------------------------------------
if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    set(CMAKE_MSVC_RUNTIME_LIBRARY
        $<$<CONFIG:Debug>:MultiThreadedDebugDLL>$<$<NOT:$<CONFIG:Debug>>:MultiThreadedDLL>
    )
endif()

# ------------------------------------------------------------------------------
# Unified UTF-8 policy
#
# This file is included before Horizon is added, so the workspace source tree
# and all CoronaEngine subdirectories inherit the same policy. Horizon's legacy
# long-form charset options are normalized in corona_third_party.cmake.
#
# We deliberately use the shortcut /utf-8 (== /source-charset:utf-8 +
# /execution-charset:utf-8) because OpenUSD also injects /utf-8 on every pxr
# target. Mixing /utf-8 with the long form triggers MSVC D8016.
#
# COMPILE_LANGUAGE filter prevents the flag from being forwarded to nvcc when
# building CUDA device code. For CUDA we additionally route /utf-8 to the
# host compiler (cl.exe) via -Xcompiler so that non-ASCII literals in the
# host portion of .cu files are also interpreted as UTF-8.
# ------------------------------------------------------------------------------
if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    add_compile_options(
        $<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>
        $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/utf-8>
    )
endif()

# ------------------------------------------------------------------------------
# Summary
# ------------------------------------------------------------------------------
message(STATUS "[Compile] MSVC runtime=${CMAKE_MSVC_RUNTIME_LIBRARY}")
message(STATUS "[Compile] Charset=UTF-8 via /utf-8 (C/CXX only, downstream of this file)")
