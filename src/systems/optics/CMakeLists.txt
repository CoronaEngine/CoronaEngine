# ==============================================================================
# Optics System
# ==============================================================================

corona_add_system(optics
    SOURCES
        optics_system.cpp
        actor_pick_readback_worker.cpp
        ${PROJECT_SOURCE_DIR}/include/corona/systems/optics/actor_pick_readback_worker.h
        actor_pick_result.h
        hardware.h
        $<$<BOOL:${CORONA_BUILD_VISION}>:vision/vision_material_adapter.cpp>
        $<$<BOOL:${CORONA_BUILD_VISION}>:vision/vision_material_adapter.h>
        $<$<BOOL:${CORONA_BUILD_VISION}>:vision/vision_geometry_adapter.cpp>
        $<$<BOOL:${CORONA_BUILD_VISION}>:vision/vision_geometry_adapter.h>
        $<$<BOOL:${CORONA_BUILD_VISION}>:vision/vision_camera_adapter.cpp>
        $<$<BOOL:${CORONA_BUILD_VISION}>:vision/vision_camera_adapter.h>
        $<$<BOOL:${CORONA_BUILD_VISION}>:vision/vision_light_adapter.cpp>
        $<$<BOOL:${CORONA_BUILD_VISION}>:vision/vision_light_adapter.h>
        $<$<BOOL:${CORONA_BUILD_VISION}>:vision/vision_render_mode_config.cpp>
        $<$<BOOL:${CORONA_BUILD_VISION}>:vision/vision_render_mode_config.h>
        $<$<BOOL:${CORONA_BUILD_VISION}>:${PROJECT_SOURCE_DIR}/include/corona/systems/optics/vision_pipeline_key.h>
        $<$<BOOL:${CORONA_BUILD_VISION}>:${PROJECT_SOURCE_DIR}/include/corona/systems/optics/vision_scene_resource.h>
        $<$<BOOL:${CORONA_BUILD_VISION}>:vision/vision_interop_lifetime.h>
        $<$<BOOL:${CORONA_BUILD_VISION}>:vision/vision_zero_copy_bridge.cpp>
        $<$<BOOL:${CORONA_BUILD_VISION}>:vision/vision_zero_copy_bridge.h>
    DEPENDENCIES
        Horizon
        corona::resource::manager
    INCLUDE_DIRS
    	$<$<BOOL:${CORONA_BUILD_VISION}>:${CMAKE_SOURCE_DIR}/vision/src>
)

# vision-all_libs is linked PRIVATEly here on purpose: vision bundles its own
# copy of Dear ImGui v1.80 under vision/src/ext/window/imgui and exposes it via
# PUBLIC include directories on vision-window-imgui. If propagated transitively
# to CoronaEngine, that include path is searched before build/_deps/imgui-src
# (v1.92.5), causing <imgui.h> to resolve to the old bundled copy and breaking
# headers that depend on newer types such as ImGuiViewport (C2061). The optics
# system's public headers do not use any vision symbols, so PRIVATE linkage is
# sufficient.
if(CORONA_BUILD_VISION)
    target_link_libraries(corona_optics_system PRIVATE vision-all_libs)
endif()

set(OPTICS_CURSOR_SHADER "${PROJECT_SOURCE_DIR}/assets/shaders/optics_cursor.comp.glsl")
set_source_files_properties("${OPTICS_CURSOR_SHADER}" PROPERTIES HEADER_FILE_ONLY TRUE)
target_sources(corona_optics_system PRIVATE "${OPTICS_CURSOR_SHADER}")
set(OPTICS_GIZMO_SHADER "${PROJECT_SOURCE_DIR}/assets/shaders/optics_gizmo.comp.glsl")
set_source_files_properties("${OPTICS_GIZMO_SHADER}" PROPERTIES HEADER_FILE_ONLY TRUE)
target_sources(corona_optics_system PRIVATE "${OPTICS_GIZMO_SHADER}")
# The shader is included through Helicon's GLSL(...) macro in hardware.h.  Keep
# an explicit object dependency as well, so changing the GLSL source always
# recompiles the optics translation unit even when the compiler's generated
# header dependency scanner misses the macro-expanded include.
set_source_files_properties(
    optics_system.cpp
    hardware.h
    PROPERTIES OBJECT_DEPENDS "${OPTICS_GIZMO_SHADER}"
)

# The Vision-enabled OpticsSystem translation unit exceeds MSVC's default COFF
# section limit in Debug builds. Restrict /bigobj to that compiler and source
# file so other toolchains and compilation units retain their normal flags.
if(MSVC)
    set_source_files_properties(
        optics_system.cpp
        PROPERTIES COMPILE_OPTIONS "/bigobj"
    )
endif()



# ------------------------------------------------------------------------------
# Vision import-scene verification demo (OFF by default)
# When enabled (and Vision is built), the optics system loads a known-good scene
# straight from disk via vision::Importer::import_scene to verify that the Vision
# backend can produce a picture in isolation, bypassing the CoronaEngine->Vision
# scene-building adapters. Edit kVisionDemoScenePath in optics_system.cpp to point
# at a local *.json scene.
# ------------------------------------------------------------------------------
option(CORONA_VISION_IMPORT_DEMO "Enable the Vision import-scene verification demo in OpticsSystem" OFF)
if(CORONA_BUILD_VISION AND CORONA_VISION_IMPORT_DEMO)
    target_compile_definitions(corona_optics_system PRIVATE CORONA_VISION_IMPORT_DEMO)
endif()


helicon_compile_shaders(corona_optics_system
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders"
)
