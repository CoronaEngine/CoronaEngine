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
# Lock timeout diagnostics
#
# Horizon's Storage can report a slot lock held longer than CFW_LOCK_TIMEOUT_MS
# with a CRITICAL log, a captured stack and a thrown exception instead of waiting
# forever. That code is compiled out by default, which is why a stuck engine lock
# is indistinguishable from an idle system in the logs: both the holder and the
# waiter just stop reporting.
#
# Turn this on for development/soak builds so a hang becomes a loud failure with a
# stack instead of a silent freeze. It is a diagnostics switch, not a shipping
# configuration: a legitimately long hold (large parse, GPU upload, disk read under
# a Scene lock) beyond the threshold aborts the holder's update(), and SystemBase
# treats an update() exception as fatal (it requests engine shutdown). The default
# threshold is deliberately generous.
# ------------------------------------------------------------------------------
option(CORONA_ENABLE_LOCK_TIMEOUT_DIAGNOSTICS
       "Report Storage lock waits longer than CFW_LOCK_TIMEOUT_MS with a stack (development only)" OFF)
set(CORONA_LOCK_TIMEOUT_MS "5000"
    CACHE STRING "Lock wait threshold in ms used when CORONA_ENABLE_LOCK_TIMEOUT_DIAGNOSTICS is ON")

# ------------------------------------------------------------------------------
# Vision is a required part of every engine build.
# Vulkan and Slang requirements are validated by the Ocarina target.
# ------------------------------------------------------------------------------
# Standalone apps, additional tests and offline filtering remain optional.
option(VISION_BUILD_APPS    "Build Vision sample apps (vision-gui, viewers, ...)" ON)
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
message(STATUS "[Options] VISION_BUILD_APPS                       = ${VISION_BUILD_APPS}")
message(STATUS "[Options] VISION_BUILD_TESTS                      = ${VISION_BUILD_TESTS}")
message(STATUS "[Options] VISION_BUILD_OIDN                       = ${VISION_BUILD_OIDN}")
