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

if(BUILD_CORONA_TESTING OR BUILD_TESTING)
    add_executable(corona_actor_selection_routing_tests
        tests/test_actor_selection_routing.cpp
    )
    target_include_directories(corona_actor_selection_routing_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(corona_actor_selection_routing_tests
        PRIVATE nlohmann_json::nlohmann_json
    )
    add_test(NAME ActorSelectionRoutingTests
             COMMAND corona_actor_selection_routing_tests)

    add_executable(corona_scene_folder_tests
        tests/test_scene_folder.cpp
        cef/scene_folder.cpp
    )
    target_include_directories(corona_scene_folder_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(corona_scene_folder_tests
        PRIVATE nlohmann_json::nlohmann_json bcrypt assimp::assimp
    )
    target_compile_definitions(corona_scene_folder_tests PRIVATE
        CORONA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    if(MSVC)
        target_compile_options(corona_scene_folder_tests PRIVATE /EHsc)
    endif()
    add_test(NAME SceneFolderTests COMMAND corona_scene_folder_tests)

    add_executable(corona_cef_app_tests
        tests/test_cef_app.cpp
    )
    target_include_directories(corona_cef_app_tests
        PRIVATE ${PROJECT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(corona_cef_app_tests
        PRIVATE corona_ui_system
    )
    if(COMMAND helicon_install_runtime_deps)
        helicon_install_runtime_deps(corona_cef_app_tests)
    endif()
    add_custom_command(TARGET corona_cef_app_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<IF:$<CONFIG:Debug>,${CEF_ROOT}/Debug/libcef.dll,${CEF_ROOT}/Release/libcef.dll>"
                "$<TARGET_FILE_DIR:corona_cef_app_tests>/libcef.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<IF:$<CONFIG:Debug>,${CEF_ROOT}/Debug/chrome_elf.dll,${CEF_ROOT}/Release/chrome_elf.dll>"
                "$<TARGET_FILE_DIR:corona_cef_app_tests>/chrome_elf.dll"
        VERBATIM
    )
    add_test(NAME CefAppTests COMMAND corona_cef_app_tests)

    add_executable(corona_collaborative_editor_loopback_tests
        tests/test_collaborative_editor_loopback.cpp
        ${PROJECT_SOURCE_DIR}/src/shared_data_hub.cpp
    )
    target_include_directories(corona_collaborative_editor_loopback_tests
        PRIVATE ${PROJECT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(corona_collaborative_editor_loopback_tests
        PRIVATE corona_ui_system
    )
    if(COMMAND helicon_install_runtime_deps)
        helicon_install_runtime_deps(corona_collaborative_editor_loopback_tests)
    endif()
    add_custom_command(TARGET corona_collaborative_editor_loopback_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<IF:$<CONFIG:Debug>,${CEF_ROOT}/Debug/libcef.dll,${CEF_ROOT}/Release/libcef.dll>"
                "$<TARGET_FILE_DIR:corona_collaborative_editor_loopback_tests>/libcef.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<IF:$<CONFIG:Debug>,${CEF_ROOT}/Debug/chrome_elf.dll,${CEF_ROOT}/Release/chrome_elf.dll>"
                "$<TARGET_FILE_DIR:corona_collaborative_editor_loopback_tests>/chrome_elf.dll"
        VERBATIM
    )
    add_test(NAME CollaborativeEditorLoopbackTests
             COMMAND corona_collaborative_editor_loopback_tests)
    get_filename_component(_CORONA_LOOPBACK_PYTHON_DIR "${Python_EXECUTABLE}" DIRECTORY)
    set(_CORONA_LOOPBACK_PATH_DIRS "${_CORONA_LOOPBACK_PYTHON_DIR}")
    foreach(_tbb_runtime IN LISTS CORONA_TBB_REDIS_DLLS)
        get_filename_component(_tbb_runtime_dir "${_tbb_runtime}" DIRECTORY)
        list(APPEND _CORONA_LOOPBACK_PATH_DIRS "${_tbb_runtime_dir}")
    endforeach()
    list(REMOVE_DUPLICATES _CORONA_LOOPBACK_PATH_DIRS)
    set(_CORONA_LOOPBACK_ENVIRONMENT)
    foreach(_runtime_dir IN LISTS _CORONA_LOOPBACK_PATH_DIRS)
        list(APPEND _CORONA_LOOPBACK_ENVIRONMENT
             "PATH=path_list_prepend:${_runtime_dir}")
    endforeach()
    set_tests_properties(CollaborativeEditorLoopbackTests PROPERTIES
        RUN_SERIAL TRUE
        TIMEOUT 30
        ENVIRONMENT_MODIFICATION "${_CORONA_LOOPBACK_ENVIRONMENT}"
    )
    unset(_CORONA_LOOPBACK_PYTHON_DIR)
    unset(_CORONA_LOOPBACK_PATH_DIRS)
    unset(_CORONA_LOOPBACK_ENVIRONMENT)
    unset(_runtime_dir)
    unset(_tbb_runtime_dir)

    add_executable(corona_ui_surface_lifecycle_tests
        tests/test_ui_surface_lifecycle.cpp
        ui_surface_lifecycle.cpp
    )
    target_include_directories(corona_ui_surface_lifecycle_tests
        PRIVATE ${PROJECT_SOURCE_DIR}/include
    )
    add_test(NAME UiSurfaceLifecycleTests
             COMMAND corona_ui_surface_lifecycle_tests)

    add_executable(corona_camera_viewport_manager_tests
        tests/test_camera_viewport_manager.cpp
    )
    target_include_directories(corona_camera_viewport_manager_tests
        PRIVATE ${PROJECT_SOURCE_DIR}/include
    )
    add_test(NAME CameraViewportManagerTests
             COMMAND corona_camera_viewport_manager_tests)

    add_executable(corona_vision_actor_material_bridge_tests
        tests/test_vision_actor_material_bridge.cpp
        cef/vision_actor_material_bridge.cpp
    )
    target_include_directories(corona_vision_actor_material_bridge_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(corona_vision_actor_material_bridge_tests
        PRIVATE nlohmann_json::nlohmann_json
    )
    add_test(NAME VisionActorMaterialBridgeTests
             COMMAND corona_vision_actor_material_bridge_tests)

    add_executable(corona_vision_actor_transform_bridge_tests
        tests/test_vision_actor_transform_bridge.cpp
        cef/vision_actor_transform_bridge.cpp
      )
    target_include_directories(corona_vision_actor_transform_bridge_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(corona_vision_actor_transform_bridge_tests
        PRIVATE nlohmann_json::nlohmann_json
    )
    add_test(NAME VisionActorTransformBridgeTests
             COMMAND corona_vision_actor_transform_bridge_tests)

    add_executable(corona_request_response_broker_tests
        tests/test_request_response_broker.cpp
        cef/request_response_broker.cpp
    )
    target_include_directories(corona_request_response_broker_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cef
    )
    target_link_libraries(corona_request_response_broker_tests
        PRIVATE Horizon nlohmann_json::nlohmann_json
    )
    add_test(NAME RequestResponseBrokerTests
             COMMAND corona_request_response_broker_tests)

    add_executable(corona_viewport_gizmo_state_tests
        tests/test_viewport_gizmo_state.cpp
        ${PROJECT_SOURCE_DIR}/src/shared_data_hub.cpp
    )
    target_include_directories(corona_viewport_gizmo_state_tests
        PRIVATE ${PROJECT_SOURCE_DIR}/include
    )
    target_link_libraries(corona_viewport_gizmo_state_tests PRIVATE Horizon)
    add_test(NAME ViewportGizmoStateTests
             COMMAND corona_viewport_gizmo_state_tests)

    add_executable(corona_ui_multisurface_smoke
        tests/ui_multisurface_smoke.cpp
        ${PROJECT_SOURCE_DIR}/src/shared_data_hub.cpp
    )
    target_include_directories(corona_ui_multisurface_smoke
        PRIVATE ${PROJECT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(corona_ui_multisurface_smoke
        PRIVATE corona_ui_system corona_display_system corona_script_system
    )
    if(COMMAND helicon_install_runtime_deps)
        helicon_install_runtime_deps(corona_ui_multisurface_smoke)
    endif()
    add_test(NAME UiMultiSurfaceSmoke
             COMMAND corona_ui_multisurface_smoke)
    set_tests_properties(UiMultiSurfaceSmoke PROPERTIES
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77
        TIMEOUT 180
    )
endif()
