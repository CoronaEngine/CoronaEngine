# ============================================================================== 
# CoronaEngine - Root CMake Configuration
#
# Responsibilities:
#   - Coordinate the build using modular helper scripts located in misc/cmake.
#   - Register project-wide options, toolchains, third-party dependencies, and
#     runtime asset staging helpers.
#   - Add subdirectories in module order: src -> engine -> examples.
# ============================================================================== 

cmake_minimum_required(VERSION 3.29)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/misc/cmake")

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# ------------------------------------------------------------------------------
# Project Declaration
# ------------------------------------------------------------------------------
project(
    CoronaEngine
    VERSION      0.5.0
    HOMEPAGE_URL "https://github.com/CoronaEngine/CoronaEngine"
    LANGUAGES    C CXX
)

set(_CORONA_DEFAULT_FETCHCONTENT_BASE "${CMAKE_BINARY_DIR}/_deps")
set(_CORONA_FETCHCONTENT_BASE_IS_STALE_HORIZON OFF)

if(DEFINED FETCHCONTENT_BASE_DIR AND NOT FETCHCONTENT_BASE_DIR STREQUAL "")
    cmake_path(
        ABSOLUTE_PATH FETCHCONTENT_BASE_DIR
        BASE_DIRECTORY "${CMAKE_BINARY_DIR}"
        NORMALIZE
        OUTPUT_VARIABLE _CORONA_FETCHCONTENT_BASE_ABSOLUTE
    )

    if(_CORONA_FETCHCONTENT_BASE_ABSOLUTE MATCHES [=[[/\\]horizon-src[/\\]build[/\\]_deps$]=])
        set(_CORONA_FETCHCONTENT_BASE_IS_STALE_HORIZON ON)
    endif()
endif()
unset(_CORONA_LOCAL_HORIZON_SOURCE)
unset(_CORONA_OFFLINE_FETCH_BASE)

if(NOT DEFINED FETCHCONTENT_BASE_DIR
   OR FETCHCONTENT_BASE_DIR STREQUAL ""
   OR _CORONA_FETCHCONTENT_BASE_IS_STALE_HORIZON)
    set(FETCHCONTENT_BASE_DIR "${_CORONA_DEFAULT_FETCHCONTENT_BASE}" CACHE PATH
        "FetchContent source cache directory" FORCE)
    message(STATUS "[FetchContent] Source cache: ${FETCHCONTENT_BASE_DIR}")
elseif(DEFINED _CORONA_FETCHCONTENT_BASE_ABSOLUTE)
    set(FETCHCONTENT_BASE_DIR "${_CORONA_FETCHCONTENT_BASE_ABSOLUTE}" CACHE PATH
        "FetchContent source cache directory" FORCE)
    message(STATUS "[FetchContent] Source cache: ${_CORONA_FETCHCONTENT_BASE_ABSOLUTE}")
endif()

unset(_CORONA_DEFAULT_FETCHCONTENT_BASE)
unset(_CORONA_FETCHCONTENT_BASE_ABSOLUTE)
unset(_CORONA_FETCHCONTENT_BASE_IS_STALE_HORIZON)

# ------------------------------------------------------------------------------
# Modular Configuration Includes
# ------------------------------------------------------------------------------
include(corona_options)             # Global build options (editor/examples/runtime switches)
include(corona_python)              # Embedded Python discovery and dependency checks
include(corona_third_party)         # FetchContent declarations for external dependencies
include(corona_tbb)                 # Engine-owned TBB discovery and runtime copy helpers
include(corona_compile_config)      # Compiler settings, standards, and shared definitions
include(corona_add_system)          # Helper for adding system modules
include(corona_runtime_deps)        # Runtime dependency collection and copy helpers
include(corona_editor)              # Editor asset packaging helpers
include(corona_cef)

# ------------------------------------------------------------------------------
# Testing Support
# ------------------------------------------------------------------------------
include(CTest)
if(BUILD_CORONA_TESTING AND NOT BUILD_TESTING)
    enable_testing()
    set(CORONA_RESOURCE_BUILD_TESTS ON CACHE BOOL "Build CoronaResource tests" FORCE)
endif()

# ------------------------------------------------------------------------------
# Subdirectories
# ------------------------------------------------------------------------------
add_subdirectory(modules/corona_resource)  # Resource management module
add_subdirectory(src)               # Core engine static library

if(CORONA_BUILD_VISION)
    add_subdirectory(vision)        # Vision renderer module (local source)
endif()

if(BUILD_CORONA_EXAMPLES)
    add_subdirectory(examples)      # Example applications
endif()

# ------------------------------------------------------------------------------
# Copy sync-deps.bat to _deps directory
# ------------------------------------------------------------------------------
# Use FETCHCONTENT_BASE_DIR if set, otherwise fall back to ${CMAKE_BINARY_DIR}/_deps
if(FETCHCONTENT_BASE_DIR)
    set(_DEPS_DIR "${FETCHCONTENT_BASE_DIR}")
else()
    set(_DEPS_DIR "${CMAKE_BINARY_DIR}/_deps")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sync-deps.bat")
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/sync-deps.bat"
         DESTINATION "${_DEPS_DIR}")
    message(STATUS "Copied sync-deps.bat to ${_DEPS_DIR}")
endif()

# ------------------------------------------------------------------------------
# Configuration Summary
# ------------------------------------------------------------------------------
message(STATUS "")
message(STATUS "================================================================================")
message(STATUS "  CoronaEngine Configuration Summary")
message(STATUS "================================================================================")
message(STATUS "  Python executable     : ${Python_EXECUTABLE}")

if(Python_VERSION)
    message(STATUS "  Python version        : ${Python_VERSION}")
endif()

message(STATUS "  Build runtime         : ${BUILD_CORONA_RUNTIME}")
message(STATUS "  Build examples        : ${BUILD_CORONA_EXAMPLES}")
message(STATUS "  Build editor          : ${BUILD_CORONA_EDITOR}")
message(STATUS "  Auto install deps     : ${CORONA_AUTO_INSTALL_PY_DEPS}")
message(STATUS "================================================================================")
message(STATUS "")
