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
# Helper: render a CMake path as a C string literal body
#
# `corona_c_string_path(INPUT out_var)`
#
# CMake always stores paths with forward slashes, but a literal handed to Win32
# through std::filesystem is expected in native form, so Windows also gets the
# separator conversion. POSIX keeps its forward slashes and only needs escaping;
# converting them would produce paths like "\usr\local" that no runtime accepts.
# ------------------------------------------------------------------------------
function(corona_c_string_path INPUT OUT_VAR)
    set(_val "${INPUT}")

    if(WIN32)
        string(REPLACE "/" "\\" _val "${_val}")
    endif()

    # Escape for a C string literal: backslashes first, then quotes.
    string(REPLACE "\\" "\\\\" _val "${_val}")
    string(REPLACE "\"" "\\\"" _val "${_val}")

    set(${OUT_VAR} "${_val}" PARENT_SCOPE)
endfunction()

# ------------------------------------------------------------------------------
# Python path macro definitions
#
# CPython's on-disk layout is platform-specific: Windows keeps the standard
# library in Lib/ and the extension modules in DLLs/, while POSIX keeps both
# below lib/pythonX.Y/ (extension modules in lib-dynload/). The conda fallback
# and a packaged runtime both follow the host's layout, so the values are
# computed here instead of being fixed to the Windows names. The version comes
# from the interpreter find_package(Python) already resolved.
# ------------------------------------------------------------------------------
set(_CORONA_PY_VERSION "${Python_VERSION_MAJOR}.${Python_VERSION_MINOR}")

# Python_RUNTIME_LIBRARY_DIRS is only defined on Windows; POSIX derives the same
# environment root from Python_ROOT_DIR, which corona_python.cmake pins to the
# conda environment.
if(WIN32 AND DEFINED Python_RUNTIME_LIBRARY_DIRS AND NOT Python_RUNTIME_LIBRARY_DIRS STREQUAL "")
    set(_CORONA_PY_HOME "${Python_RUNTIME_LIBRARY_DIRS}")
else()
    set(_CORONA_PY_HOME "${Python_ROOT_DIR}")
endif()

if(WIN32)
    set(_CORONA_PY_STDLIB "${_CORONA_PY_HOME}/Lib")
    set(_CORONA_PY_EXT_MODULES "${_CORONA_PY_HOME}/DLLs")
else()
    set(_CORONA_PY_STDLIB "${_CORONA_PY_HOME}/lib/python${_CORONA_PY_VERSION}")
    set(_CORONA_PY_EXT_MODULES "${_CORONA_PY_STDLIB}/lib-dynload")
endif()

corona_c_string_path("${_CORONA_PY_HOME}" _CORONA_PY_HOME_ESC)
corona_c_string_path("${_CORONA_PY_STDLIB}" _CORONA_PY_STDLIB_ESC)
corona_c_string_path("${_CORONA_PY_EXT_MODULES}" _CORONA_PY_EXT_MODULES_ESC)
corona_c_string_path("${_CORONA_PY_VERSION}" _CORONA_PY_VERSION_ESC)

# ------------------------------------------------------------------------------
# Global compile definitions
# ------------------------------------------------------------------------------
add_compile_definitions(
    $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>

    NOMINMAX

    ENTT_ID_TYPE=uint64_t

    ENTT_USE_ATOMIC

    CORONA_PYTHON_HOME_DIR=\"${_CORONA_PY_HOME_ESC}\"
    CORONA_PYTHON_STDLIB_DIR=\"${_CORONA_PY_STDLIB_ESC}\"
    CORONA_PYTHON_EXT_MODULES_DIR=\"${_CORONA_PY_EXT_MODULES_ESC}\"
    CORONA_PYTHON_VERSION=\"${_CORONA_PY_VERSION_ESC}\"
)

if(CORONA_BUILD_HARDWARE)
    add_compile_definitions(CORONA_ENABLE_HARDWARE)
endif()

# Enable Python API macros only when building the editor
if(BUILD_CORONA_EDITOR)
    add_compile_definitions(CORONA_ENABLE_PYTHON_API)
endif()

# Lock timeout diagnostics (see corona_options.cmake). Applied here — before the
# project and the Horizon workspace are added — so every translation unit that
# inlines horizon/core/storage.h agrees on the definition; mixing the two settings
# across targets would be an ODR violation of the same inline functions.
if(CORONA_ENABLE_LOCK_TIMEOUT_DIAGNOSTICS)
    add_compile_definitions(
        CFW_ENABLE_LOCK_TIMEOUT=1
        CFW_LOCK_TIMEOUT_MS=${CORONA_LOCK_TIMEOUT_MS}
    )
    message(STATUS "[Compile] Lock timeout diagnostics enabled (CFW_LOCK_TIMEOUT_MS=${CORONA_LOCK_TIMEOUT_MS})")
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
# Apply UTF-8 to host C/C++ compilation.
# ------------------------------------------------------------------------------
if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    add_compile_options(
        $<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>
    )
endif()

# ------------------------------------------------------------------------------
# Summary
# ------------------------------------------------------------------------------
message(STATUS "[Compile] MSVC runtime=${CMAKE_MSVC_RUNTIME_LIBRARY}")
message(STATUS "[Compile] Charset=UTF-8 via /utf-8 (C/CXX only, downstream of this file)")
