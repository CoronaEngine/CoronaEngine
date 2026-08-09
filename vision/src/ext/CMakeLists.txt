# ==============================================================================
# Vision external dependency glue
#
# Vision was originally a stand-alone project that pulled cxxopts / assimp /
# glfw / oidn as git submodules under this directory. After integration into
# CoronaEngine those dependencies are resolved once at the engine root via
# Conan package targets (see cmake/corona_third_party.cmake). This file now
# only:
#   - asserts the expected upstream targets exist;
#   - defines small INTERFACE wrappers (`vision-ext`, `vision-ext-assimp_config`,
#     `vision-ext-oidn`, `vision-ext-xatlas`) so the rest of the Vision tree can
#     keep linking against stable names;
#   - keeps Vision-local in-tree libraries (xatlas, vision-window-* in ./window).
# ==============================================================================

if (MSVC)
    add_compile_options(/wd4819)
endif ()

function(vision_target_accepts_private_properties out_var target_name)
    if (NOT TARGET ${target_name})
        set(${out_var} FALSE PARENT_SCOPE)
        return()
    endif ()

    get_target_property(_vision_target_type ${target_name} TYPE)
    get_target_property(_vision_target_imported ${target_name} IMPORTED)
    if (_vision_target_imported OR _vision_target_type STREQUAL "INTERFACE_LIBRARY")
        set(${out_var} FALSE PARENT_SCOPE)
    else ()
        set(${out_var} TRUE PARENT_SCOPE)
    endif ()
endfunction()

# ------------------------------------------------------------------------------
# cxxopts (Conan package target at engine root)
# ------------------------------------------------------------------------------
if (NOT TARGET cxxopts)
    message(FATAL_ERROR "[vision/ext] Expected target 'cxxopts' from CoronaEngine Conan dependencies. "
                        "Make sure corona_third_party.cmake has been processed before vision/.")
endif ()

# ------------------------------------------------------------------------------
# assimp (Conan package target at engine root)
# ------------------------------------------------------------------------------
if (NOT TARGET assimp)
    message(FATAL_ERROR "[vision/ext] Expected target 'assimp' from CoronaEngine Conan dependencies.")
endif ()

if (TARGET zlibstatic AND WIN32)
    target_compile_definitions(zlibstatic PRIVATE
            _CRT_SECURE_NO_WARNINGS
            _CRT_NONSTDC_NO_DEPRECATE)
endif ()

if (TARGET zlibstatic AND CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(zlibstatic PRIVATE -Wno-deprecated-non-prototype)
endif ()

vision_target_accepts_private_properties(_vision_mutable_assimp assimp)
if (_vision_mutable_assimp)
    set_target_properties(assimp PROPERTIES UNITY_BUILD OFF)
    if (UNIX AND NOT APPLE)
        target_compile_definitions(assimp PRIVATE USE_FILE32API=1)
    endif ()
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND
            CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 15.0)
        target_compile_options(assimp PRIVATE -Wno-deprecated-non-prototype)
    endif ()
endif ()

add_library(vision-ext-assimp_config INTERFACE)
target_link_libraries(vision-ext-assimp_config INTERFACE assimp::assimp)

# ------------------------------------------------------------------------------
# glfw (Conan package target at engine root)
# ------------------------------------------------------------------------------
if (NOT TARGET glfw)
    message(FATAL_ERROR "[vision/ext] Expected target 'glfw' from CoronaEngine Conan dependencies.")
endif ()

vision_target_accepts_private_properties(_vision_mutable_glfw glfw)
if (_vision_mutable_glfw AND WIN32)
    target_compile_definitions(glfw PRIVATE
            _CRT_SECURE_NO_WARNINGS
            _CRT_NONSTDC_NO_DEPRECATE)
endif ()

# ------------------------------------------------------------------------------
# OIDN (optional Conan package target at engine root, gated by VISION_BUILD_OIDN)
# ------------------------------------------------------------------------------
if (VISION_BUILD_OIDN)
    if (NOT TARGET OpenImageDenoise)
        message(FATAL_ERROR "[vision/ext] VISION_BUILD_OIDN=ON but target 'OpenImageDenoise' not found.")
    endif ()

    foreach (_oidn_tgt
            OpenImageDenoise
            OpenImageDenoise_core
            OpenImageDenoise_device_cpu
            OpenImageDenoise_device_cuda)
        if (TARGET ${_oidn_tgt})
            set_target_properties(${_oidn_tgt} PROPERTIES
                    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/)
        endif ()
    endforeach ()

    add_library(vision-ext-oidn INTERFACE)
    target_link_libraries(vision-ext-oidn INTERFACE OpenImageDenoise)
endif ()

# ------------------------------------------------------------------------------
# In-tree dependencies kept inside Vision
# ------------------------------------------------------------------------------
add_subdirectory(xatlas)
add_library(vision-ext-xatlas INTERFACE)
target_link_libraries(vision-ext-xatlas INTERFACE xatlas)

# Vision keeps its own imgui+glad+ImGuizmo bundle (target: vision-window-imgui).
# That bundle is intentionally separate from CoronaEngine's global `imgui`
# target so version drift between the two trees does not break either side.
add_subdirectory(window)

# Aggregated INTERFACE consumed by vision-base etc.
add_library(vision-ext INTERFACE)
target_link_libraries(vision-ext INTERFACE cxxopts)
