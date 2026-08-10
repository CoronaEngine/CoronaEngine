# ==============================================================================
# corona_cef.cmake
#
# CEF is provided by the Conan package cef-binary. CMake only consumes the
# unpacked CEF root and builds libcef_dll_wrapper from that package.
# ==============================================================================

include_guard(GLOBAL)

option(CORONA_ENABLE_CEF "Enable CEF integration for CoronaEngine" ON)

set(CEF_AVAILABLE OFF)
if(NOT CORONA_ENABLE_CEF)
    message(STATUS "CEF: disabled")
    return()
endif()

if(NOT DEFINED CORONA_CEF_ROOT OR CORONA_CEF_ROOT STREQUAL "")
    find_package(cef-binary CONFIG REQUIRED)
    if(DEFINED cef-binary_INCLUDE_DIR AND NOT cef-binary_INCLUDE_DIR STREQUAL "")
        get_filename_component(CORONA_CEF_ROOT "${cef-binary_INCLUDE_DIR}" DIRECTORY)
    endif()
endif()

if(NOT DEFINED CORONA_CEF_ROOT OR CORONA_CEF_ROOT STREQUAL "")
    message(FATAL_ERROR
        "CEF is enabled but CORONA_CEF_ROOT is not set. "
        "Run 'conda run -n coronaengine-dev python tools/dev.py configure' so cef-binary provides it.")
endif()

get_filename_component(CEF_ROOT "${CORONA_CEF_ROOT}" ABSOLUTE)
set(CEF_INCLUDE_DIR "${CEF_ROOT}/include")
set(CEF_RES_DIR "${CEF_ROOT}/Resources")

if(NOT EXISTS "${CEF_INCLUDE_DIR}/cef_base.h")
    message(FATAL_ERROR "CEF headers not found: expected ${CEF_INCLUDE_DIR}/cef_base.h")
endif()

if(NOT EXISTS "${CEF_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "CEF CMakeLists.txt not found: expected ${CEF_ROOT}/CMakeLists.txt")
endif()

set(CEF_AVAILABLE ON)
message(STATUS "CEF: using Conan package root ${CEF_ROOT}")

# CEF's cef_variables.cmake appends 'd' for Debug automatically:
# /MD -> /MDd for Debug, /MD for non-Debug configs.
set(CEF_RUNTIME_LIBRARY_FLAG "/MD" CACHE STRING "Use dynamic CRT (/MD) for CEF wrapper" FORCE)
add_subdirectory("${CEF_ROOT}" "${CMAKE_BINARY_DIR}/cef_wrapper" EXCLUDE_FROM_ALL)

if(CMAKE_CONFIGURATION_TYPES)
    set(CEF_CFG_DIR "$<CONFIG>")
else()
    set(CEF_CFG_DIR "${CMAKE_BUILD_TYPE}")
    if(CEF_CFG_DIR STREQUAL "")
        set(CEF_CFG_DIR "Release")
    endif()
endif()

set(CEF_BIN_DIR "${CEF_ROOT}/${CEF_CFG_DIR}")
set(CEF_LIBCEF_DEBUG "${CEF_ROOT}/Debug/libcef.lib")
set(CEF_LIBCEF_RELEASE "${CEF_ROOT}/Release/libcef.lib")
set(CEF_SANDBOX_LIB "${CEF_BIN_DIR}/cef_sandbox.lib")
