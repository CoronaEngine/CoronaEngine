# Public CMake entry point. Build implementation lives in build_tools/.
cmake_minimum_required(VERSION 3.29)

include("${CMAKE_CURRENT_LIST_DIR}/build_tools/cmake/corona_paths.cmake")
list(APPEND CMAKE_MODULE_PATH
    "${CORONA_BUILD_TOOLS_DIR}/cmake"
    "${CORONA_ROOT_DIR}/modules/corona_resource/cmake"
)

include(corona_dev_bootstrap)
corona_dev_bootstrap()

# Keep project() here so toolchain setup precedes compiler detection.
project(CoronaEngine
    VERSION 0.5.0
    HOMEPAGE_URL "https://github.com/CoronaEngine/CoronaEngine"
    LANGUAGES C CXX
)

include(corona_project)
