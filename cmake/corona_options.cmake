# ==============================================================================
# corona_options.cmake
#
# Purpose:
# Centralize project-level options and high-level feature toggles.
#
# Notes:
# - Consolidates boolean/path/version switches that may be provided via `-D`.
# - Provides a single place to document and extend build configuration knobs.
# - Future feature switches (tests, installers, additional third parties) should
# also be added here.
# ==============================================================================

include_guard(GLOBAL)

if(POLICY CMP0077)
    cmake_policy(SET CMP0077 NEW)
endif()

option(CORONA_AUTO_INSTALL_PY_DEPS "Auto-install missing Python packages during configure" ON)
option(BUILD_SHARED_LIBS "Build as shared libraries (default OFF for static)" OFF)
option(BUILD_CORONA_EDITOR "Build Corona editor" ON)
option(BUILD_CORONA_RUNTIME "Build Corona runtime" ON)
option(BUILD_CORONA_TESTING "Build Corona test suite" ${PROJECT_IS_TOP_LEVEL})
option(BUILD_CORONA_EXAMPLES "Build example programs" ${PROJECT_IS_TOP_LEVEL})
option(CORONA_BUILD_HARDWARE "Build Corona Hardware features" ON)

# ------------------------------------------------------------------------------
# Vision feature gate
#
# Vision requires the CUDA SDK. Mirror Horizon's behaviour (cmake/HorizonOcarina.cmake)
# and auto-detect a usable CUDA toolkit:
#   1. Respect the `CUDA_PATH` environment variable when present.
#   2. Otherwise fall back to `find_package(CUDAToolkit QUIET)`.
# Users may still force the option via `-DCORONA_BUILD_VISION=ON/OFF`.
# ------------------------------------------------------------------------------
set(_corona_vision_default OFF)
if(DEFINED ENV{CUDA_PATH} AND EXISTS "$ENV{CUDA_PATH}")
    set(_corona_vision_default ON)
    message(STATUS "[Options] CUDA SDK detected via CUDA_PATH=$ENV{CUDA_PATH}")
else()
    find_package(CUDAToolkit QUIET)
    if(CUDAToolkit_FOUND)
        set(_corona_vision_default ON)
        message(STATUS "[Options] CUDA SDK detected via find_package(CUDAToolkit) (${CUDAToolkit_VERSION})")
    endif()
endif()
option(CORONA_BUILD_VISION "Build Corona Vision features" ${_corona_vision_default})
unset(_corona_vision_default)

# If the user explicitly enabled Vision but no CUDA SDK is present, downgrade
# the option with a warning rather than failing deep in vision/ subdirectories.
if(CORONA_BUILD_VISION)
    if(NOT DEFINED ENV{CUDA_PATH} OR NOT EXISTS "$ENV{CUDA_PATH}")
        find_package(CUDAToolkit QUIET)
        if(NOT CUDAToolkit_FOUND)
            message(WARNING
                "[Options] CORONA_BUILD_VISION=ON but no CUDA SDK was found "
                "(neither CUDA_PATH env nor CUDAToolkit). Disabling Vision.")
            set(CORONA_BUILD_VISION OFF CACHE BOOL "Build Corona Vision features" FORCE)
        endif()
    endif()
endif()

# Vision sub-options (only meaningful when CORONA_BUILD_VISION=ON)
# When Vision is enabled (which already implies CUDA is present), default the
# sample apps ON so that `vision-gui` builds out-of-the-box.
option(VISION_BUILD_APPS    "Build Vision sample apps (vision-gui, viewers, ...)" ${CORONA_BUILD_VISION})
option(VISION_BUILD_TESTS   "Build Vision test executables"                       OFF)
option(VISION_BUILD_OIDN    "Fetch and build Intel Open Image Denoise for Vision" OFF)
option(VISION_UNITY_BUILD   "Enable unity build for Vision plugin sources"        OFF)
option(VISION_INTERACTIVE_AUX_WORK "Enable optional non-render work in Vision interactive mode" ON)
message(STATUS "[Options] CORONA_AUTO_INSTALL_PY_DEPS             = ${CORONA_AUTO_INSTALL_PY_DEPS}")
message(STATUS "[Options] BUILD_SHARED_LIBS                       = ${BUILD_SHARED_LIBS}")
message(STATUS "[Options] BUILD_CORONA_EDITOR                     = ${BUILD_CORONA_EDITOR}")
message(STATUS "[Options] BUILD_CORONA_RUNTIME                    = ${BUILD_CORONA_RUNTIME}")
message(STATUS "[Options] BUILD_CORONA_TESTING                    = ${BUILD_CORONA_TESTING}")
message(STATUS "[Options] BUILD_CORONA_EXAMPLES                   = ${BUILD_CORONA_EXAMPLES}")
message(STATUS "[Options] CORONA_BUILD_HARDWARE                   = ${CORONA_BUILD_HARDWARE}")
message(STATUS "[Options] CORONA_BUILD_VISION                     = ${CORONA_BUILD_VISION}")
message(STATUS "[Options] VISION_BUILD_APPS                       = ${VISION_BUILD_APPS}")
message(STATUS "[Options] VISION_BUILD_TESTS                      = ${VISION_BUILD_TESTS}")
message(STATUS "[Options] VISION_BUILD_OIDN                       = ${VISION_BUILD_OIDN}")
