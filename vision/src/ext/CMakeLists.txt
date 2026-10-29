# ==============================================================================
# Vision external dependency glue
#
# Vision was originally a stand-alone project that pulled dependencies such as
# glfw as git submodules under this directory. After integration into
# CoronaEngine those dependencies are resolved once at the engine root via
# Conan package targets (see cmake/corona_third_party.cmake). This file now
# only:
#   - asserts the expected upstream targets exist;
#   - defines an INTERFACE wrapper (`vision-ext`) so the
#     rest of the Vision tree can keep linking against stable names;
#   - keeps Vision-local window libraries (vision-window-* in ./window).
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
# nlohmann_json (Conan package target at engine root)
# ------------------------------------------------------------------------------
if (NOT TARGET nlohmann_json::nlohmann_json)
    message(FATAL_ERROR "[vision/ext] Expected target 'nlohmann_json::nlohmann_json' from CoronaEngine Conan dependencies.")
endif ()

# ------------------------------------------------------------------------------
# In-tree dependencies kept inside Vision
# ------------------------------------------------------------------------------
# Vision keeps its own imgui+glad+ImGuizmo bundle (target: vision-window-imgui).
# That bundle is intentionally separate from CoronaEngine's global `imgui`
# target so version drift between the two trees does not break either side.
add_subdirectory(window)

# Aggregated INTERFACE consumed by vision-base etc.
add_library(vision-ext INTERFACE)
target_link_libraries(vision-ext INTERFACE nlohmann_json::nlohmann_json)
