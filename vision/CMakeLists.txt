
# ==============================================================================
# Vision sub-project (integrated into CoronaEngine)
#
# This file is meant to be included via `add_subdirectory(vision)` from the
# CoronaEngine root, gated by `CORONA_BUILD_VISION`. It no longer issues its
# own `project()` call or overrides global output directories. Third-party
# dependencies (assimp, glfw, cxxopts, Horizon) are resolved at the root level
# through Conan package targets (see cmake/corona_third_party.cmake) and reused here.
# ==============================================================================

# Vision sources require C++20 and depend on CUDA.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

find_package(CUDAToolkit 11.0 REQUIRED)

add_compile_definitions(_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING)

# ------------------------------------------------------------------------------
# Output directory layout (scoped to the Vision subtree only)
#
# Vision's per-target CMakeLists reference `${CMAKE_RUNTIME_OUTPUT_DIRECTORY}`
# and `${CMAKE_LIBRARY_OUTPUT_DIRECTORY}` directly, e.g.
#     set_target_properties(<tgt> PROPERTIES
#         LIBRARY_OUTPUT_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}
#         RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
# If those variables are empty (CoronaEngine does not set them globally), CMake
# collapses the argument list and parses the property name `LIBRARY_OUTPUT_DIRECTORY`
# as if its value were the literal string `RUNTIME_OUTPUT_DIRECTORY`. That produces
# DLLs under `build/vision/.../RUNTIME_OUTPUT_DIRECTORY/<Config>/...`, while
# `vision-gui.exe` (hardcoded to `${CMAKE_BINARY_DIR}/bin/`) cannot find them at runtime.
#
# Force the same `build/bin` / `build/lib` layout used by the standalone Vision
# project. Because `set()` here is plain (no PARENT_SCOPE / CACHE), the override is
# confined to the Vision subtree and leaves CoronaEngine's other targets alone.
# ------------------------------------------------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# Bridge legacy Vision option names to the new CoronaEngine-controlled options.
# Vision sub-CMake scripts still test these legacy names; aliasing here means
# we do not have to mass-rewrite the deeper tree.
set(OIDN_ON           ${VISION_BUILD_OIDN}  CACHE INTERNAL "Vision OIDN toggle (alias of VISION_BUILD_OIDN)")
set(OCARINA_BUILD_TESTS ${VISION_BUILD_TESTS} CACHE INTERNAL "Vision test toggle (alias of VISION_BUILD_TESTS)")

# Allow Vision sources to find their own headers via `src/...`.
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)

if (MSVC)
    add_compile_options(/wd4996)
    add_compile_options(/MP)
endif ()

function(vision_add_application name)
    cmake_parse_arguments(APP "" "" "SOURCES" ${ARGN})
    add_executable(${name} ${APP_SOURCES})
    target_compile_features(${name} PRIVATE c_std_11 cxx_std_20)
    target_link_libraries(${name} PRIVATE vision-all_libs ${STDCXX_EXTRA_LIBS})
    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/")
endfunction()

message(STATUS "[Vision] CUDAToolkit found, integrating Vision sources")
message(STATUS "[Vision] VISION_BUILD_APPS  = ${VISION_BUILD_APPS}")
message(STATUS "[Vision] VISION_BUILD_TESTS = ${VISION_BUILD_TESTS}")
message(STATUS "[Vision] VISION_BUILD_OIDN  = ${VISION_BUILD_OIDN}")

add_subdirectory(src)

if (VISION_BUILD_OIDN)
    add_custom_target(CP_OIDN_CUDA ALL
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE_DIR:OpenImageDenoise_device_cuda>/OpenImageDenoise_device_cuda.dll
                    ${CMAKE_BINARY_DIR}/bin/
            DEPENDS OpenImageDenoise_device_cuda
    )
endif ()
