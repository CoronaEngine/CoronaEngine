# UI System 配置 (ImGui removed — Phase 6 of the ImGui-removal plan).
# The frame loop is now ui_frame_runner.cpp (raw SDL + quad compositor); there is no
# longer an imgui or imgui_backends (imgui_impl_sdl3) target. The imgui.vert/frag GLSL
# shaders are still used by the quad compositor and remain compiled below.
set(UI_SYSTEM_SOURCES
    ui_system.cpp
    ui_frame_runner.cpp
    ui_surface_lifecycle.cpp
    camera_viewport_manager.cpp
    panel_layout.cpp
    collaborative_editor_runtime.cpp
    collaborative_editor_runtime.h

    vulk/vulkan_backend.cpp
    vulk/quad_compositor.cpp
    vulk/browser_manager_vulkan.cpp

    cef/browser_manager.cpp
    cef/actor_selection_routing.h
    cef/browser_ui.cpp
    cef/cef_app.cpp
    cef/cef_client.cpp
    editor_api/cef_editor_api.cpp
    cef/cef_editor_native_api_registry.cpp
    cef/cef_editor_native_api_handlers.cpp
    cef/scene_folder.cpp
    cef/vision_actor_material_bridge.cpp
    cef/vision_actor_transform_bridge.cpp
    cef/cef_query_bridge.cpp
    cef/cef_realtime_bridge.cpp
    cef/request_response_broker.cpp
    cef/cef_renderer_bridge.cpp
    cef/cef_runtime.cpp
    cef/cef_py_bind.cpp
    cef/cef_osr_mode.cpp
    cef/cef_shared_texture_probe.cpp
    cef/popup_overlay.cpp

    sdl/sdl_utils.cpp
    sdl/sdl_window_manager.cpp
    sdl/sdl_input_router.cpp

    # Internal headers (listed for IDE visibility, matching other systems).
    cef/browser_manager.h
    cef/browser_ui.h
    cef/cef_app.h
    cef/cef_bridge_helpers.h
    cef/request_response_broker.h
    cef/cef_client.h
    cef/cef_editor_native_api_registry.h
    cef/cef_editor_native_api_test_support.h
    cef/cef_osr_mode.h
    cef/cef_shared_texture_probe.h
    cef/popup_overlay.h
    editor_api/cef_editor_api.h
    cef/scene_folder.h
    cef/vision_actor_material_bridge.h
    cef/vision_actor_transform_bridge.h
    cef/cef_renderer_bridge.h
    sdl/sdl_utils.h

    ${PROJECT_SOURCE_DIR}/include/corona/systems/ui/vulkan_backend.h
    ${PROJECT_SOURCE_DIR}/include/corona/systems/ui/cef_runtime.h
    # quad_compositor.h owns the `#include GLSL(ui_quad.vert/frag)` macros.
    # Helicon's shader scan is textual over the target's SOURCES (it does not
    # follow #include chains), so this header must be listed explicitly or the
    # ui_quad shader headers are never generated (C1083). Mirrors how the optics
    # system lists hardware.h.
    ${PROJECT_SOURCE_DIR}/include/corona/systems/ui/quad_compositor.h
)


corona_add_system(ui
    SOURCES ${UI_SYSTEM_SOURCES}
    DEPENDENCIES
        Horizon
        corona::system::network
        libcef_dll_wrapper
        debug "${CEF_LIBCEF_DEBUG}"
        optimized "${CEF_LIBCEF_RELEASE}"
        SDL3::SDL3-static
        ws2_32
        shlwapi
        ole32
        shell32
        dbghelp
        psapi
        version
        winmm
        bcrypt
        d3d11
        dxgi
        Python::Python
        nanobind-static
        corona::resource::manager
        assimp::assimp
        nlohmann_json::nlohmann_json
        corona::python::runtime
        corona::system::script
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/cef
        ${CMAKE_CURRENT_SOURCE_DIR}/editor_api
        ${CEF_INCLUDE_DIR}
        ${CEF_ROOT}
)

target_compile_definitions(corona_ui_system PUBLIC VK_NO_PROTOTYPES)

# The camera-follow controller is an implementation detail of the Script
# system, but the native editor handler invokes it as part of the aggregate
# editor API.  Keep its header private to UI instead of widening Script's
# public include surface.
target_include_directories(corona_ui_system PRIVATE
    ${PROJECT_SOURCE_DIR}/src/systems/script/include
)

helicon_compile_shaders(corona_ui_system
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders"
)
