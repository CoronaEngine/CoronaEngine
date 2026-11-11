# ImGui owns editor docking. Its draw data is rendered by Horizon into Display surfaces.
# Only the SDL3 platform backend is used; no stock ImGui renderer is linked.
set(UI_SYSTEM_SOURCES
    ui_system.cpp
    ui_frame_runner.cpp
    imgui/imgui_draw_packet.cpp
    imgui/imgui_renderer.cpp
    imgui/imgui_platform.cpp
    imgui/imgui_workspace.cpp
    imgui/imgui_ui.cpp
    ui_surface_lifecycle.cpp
    camera_viewport_manager.cpp
    panel_layout.cpp
    panel_resize_policy.cpp
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
    editor_api/python_service_bridge.cpp
    editor_api/scene_model_node_service.cpp
    editor_api/scene_environment_service.cpp
    editor_api/actor_properties_service.cpp
    editor_api/camera_properties_service.cpp
    editor_api/camera_render_settings_service.cpp
    editor_api/scene_projection_service.cpp
    editor_api/resource_import_service.cpp
    editor_api/project_script_service.cpp
    cef/native_api_types.cpp
    cef/editor_paths.cpp
    cef/editor_actor_projection.cpp
    cef/editor_scene_projection.cpp
    cef/editor_camera_views.cpp
    cef/editor_scene_access.cpp
    cef/editor_actor_properties.cpp
    cef/editor_network_state.cpp
    cef/editor_service_operations.cpp
    cef/cef_editor_native_api_handlers.cpp
    cef/scene_folder.cpp
    cef/scene_archive_rpc.cpp
    cef/cef_viewport_gizmo.cpp
    cef/cef_viewport_pick.cpp
    cef/cef_script_input.cpp
    cef/cef_renderer_bridge.cpp
    cef/cef_v8_binding.cpp
    cef/cef_window_operations.cpp
    cef/cef_runtime.cpp
    cef/cef_py_bind.cpp
    cef/cef_osr_mode.cpp
    cef/cef_paint_upload.cpp
    cef/cef_shared_texture_probe.cpp
    cef/popup_overlay.cpp

    sdl/sdl_utils.cpp
    sdl/sdl_window_manager.cpp
    sdl/sdl_input_router.cpp

    # Internal headers (listed for IDE visibility, matching other systems).
    cef/browser_manager.h
    cef/browser_ui.h
    cef/cef_process.h
    cef/cef_viewport_pick.h
    cef/cef_viewport_gizmo.h
    cef/cef_editor_native_api_handlers.h
    cef/cef_script_input.h
    cef/cef_client.h
    cef/native_api_types.h
    cef/editor_paths.h
    cef/editor_actor_projection.h
    cef/editor_scene_projection.h
    cef/editor_camera_views.h
    cef/editor_scene_access.h
    cef/editor_actor_properties.h
    cef/editor_network_state.h
    cef/editor_text_utils.h
    cef/native_editor_state.h
    cef/cef_editor_native_api_test_support.h
    cef/cef_osr_mode.h
    cef/cef_shared_texture_probe.h
    cef/popup_overlay.h
    editor_api/python_service_bridge.h
    editor_api/editor_api_services.h
    cef/scene_folder.h
    sdl/sdl_utils.h

    ${PROJECT_SOURCE_DIR}/include/corona/systems/ui/vulkan_backend.h
    ${PROJECT_SOURCE_DIR}/include/corona/systems/ui/cef_runtime.h
    # quad_compositor.h owns the `#include GLSL(shaders/ui_quad.vert/frag.glsl)` macros.
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
        corona::system::mechanics
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
target_link_libraries(corona_ui_system PRIVATE corona::imgui)

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
