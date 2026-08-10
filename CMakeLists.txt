# ============================================================================== 
# CoronaEngine - Root CMake Configuration
#
# Responsibilities:
#   - Coordinate the build using CMake modules in cmake and Python helpers in tools/build.
#   - Register project-wide options, toolchains, third-party dependencies, and
#     runtime asset staging helpers.
#   - Add subdirectories in module order: src -> engine -> examples.
# ============================================================================== 

cmake_minimum_required(VERSION 3.29)

list(APPEND CMAKE_MODULE_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/modules/corona_resource/cmake"
)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

include(corona_dev_bootstrap)
corona_dev_bootstrap()

# ------------------------------------------------------------------------------
# Project Declaration
# ------------------------------------------------------------------------------
project(
    CoronaEngine
    VERSION      0.5.0
    HOMEPAGE_URL "https://github.com/CoronaEngine/CoronaEngine"
    LANGUAGES    C CXX
)

# MSVC emits localized /showIncludes lines as UTF-8 when Ninja is launched
# from VS Code/CMake Tools, while CMake may cache the prefix in the Windows
# ANSI code page. The byte mismatch makes Ninja silently record zero header
# dependencies after a locked Horizon workspace update.
# Keep the generated Ninja prefix UTF-8 and deterministic.
if(MSVC)
    set(_CORONA_MSVC_SHOWINCLUDES_PREFIX "注意: 包含文件:  ")
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "${_CORONA_MSVC_SHOWINCLUDES_PREFIX}")
    set(CMAKE_C_CL_SHOWINCLUDES_PREFIX "${_CORONA_MSVC_SHOWINCLUDES_PREFIX}")
    set(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "${_CORONA_MSVC_SHOWINCLUDES_PREFIX}")
    unset(_CORONA_MSVC_SHOWINCLUDES_PREFIX)
endif()
# ------------------------------------------------------------------------------
# Modular Configuration Includes
# ------------------------------------------------------------------------------
include(corona_options)             # Global build options (editor/examples/runtime switches)
include(corona_python)              # Embedded Python discovery and dependency checks
include(corona_compile_config)      # Compiler settings, standards, and shared definitions
include(corona_third_party)         # Conan-provided external dependency targets
include(corona_tbb)                 # Engine-owned TBB discovery and runtime copy helpers
include(corona_add_system)          # Helper for adding system modules
include(corona_runtime_deps)        # Runtime dependency collection and copy helpers
include(corona_editor)              # Editor asset packaging helpers
include(corona_cef)

# ------------------------------------------------------------------------------
# Testing Support
# ------------------------------------------------------------------------------
include(CTest)
if(BUILD_CORONA_TESTING)
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
