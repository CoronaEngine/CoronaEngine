# ==============================================================================
# Optics System
# ==============================================================================

corona_add_system(optics
    SOURCES
        optics_system.cpp
        retain_submission.cpp
        hardware.h
        vision/vision_material_adapter.cpp
        vision/vision_material_adapter.h
        vision/vision_medium_adapter.cpp
        vision/vision_medium_adapter.h
        vision/vision_geometry_adapter.cpp
        vision/vision_geometry_adapter.h
        vision/vision_camera_adapter.cpp
        vision/vision_camera_adapter.h
        vision/vision_diagnostic_adapter.cpp
        vision/vision_diagnostic_adapter.h
        vision/vision_light_adapter.cpp
        vision/vision_light_adapter.h
        vision/vision_render_mode_config.cpp
        vision/vision_render_mode_config.h
        ${PROJECT_SOURCE_DIR}/include/corona/systems/optics/vision_pipeline_key.h
        ${PROJECT_SOURCE_DIR}/include/corona/systems/optics/vision_scene_resource.h
        vision/vision_interop_lifetime.h
        vision/vision_auto_exposure.h
        vision/vision_display_sampling.h
        vision/vision_skinning_bridge.cpp
        vision/vision_skinning_bridge.h
        vision/vision_shared_buffer.h
        vision/vision_zero_copy_bridge.cpp
        vision/vision_zero_copy_bridge.h
    DEPENDENCIES
        Horizon
        corona::resource::manager
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/vision/src
)

# Only the compatibility bridge sees Horizon's private command recorder. Do not
# expose this include root to Vision: both projects have headers named core/*.
set_source_files_properties(retain_submission.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/Horizon/src")
target_link_libraries(corona_optics_system PRIVATE horizon::volk)

# vision-all_libs is linked PRIVATEly here on purpose: vision bundles its own
# copy of Dear ImGui v1.80 under vision/src/ext/window/imgui and exposes it via
# PUBLIC include directories on vision-window-imgui. If propagated transitively
# to CoronaEngine, that include path is searched before build/_deps/imgui-src
# (v1.92.5), causing <imgui.h> to resolve to the old bundled copy and breaking
# headers that depend on newer types such as ImGuiViewport (C2061). The optics
# system's public headers only forward-declare Vision types, so PRIVATE linkage
# is sufficient.
target_link_libraries(corona_optics_system PRIVATE vision-all_libs)

# The OpticsSystem translation unit exceeds MSVC's default COFF
# section limit in Debug builds. Restrict /bigobj to that compiler and source
# file so other toolchains and compilation units retain their normal flags.
if(MSVC)
    set_source_files_properties(
        optics_system.cpp
        PROPERTIES COMPILE_OPTIONS "/bigobj"
    )
endif()

helicon_compile_shaders(corona_optics_system
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders"
)
