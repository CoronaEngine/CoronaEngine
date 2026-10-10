add_executable(corona_vision_camera_direction_tests
    test_vision_camera_direction.cpp
)

add_executable(corona_ik_chain_config_tests test_ik_chain_config.cpp)
target_include_directories(corona_ik_chain_config_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/systems/ui)
target_link_libraries(corona_ik_chain_config_tests PRIVATE nlohmann_json::nlohmann_json corona::resource::manager)
add_test(NAME IkChainConfigTests COMMAND corona_ik_chain_config_tests)
target_include_directories(corona_vision_camera_direction_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/src/systems/ui
)
target_link_libraries(corona_vision_camera_direction_tests
    PRIVATE nlohmann_json::nlohmann_json
)
add_test(NAME VisionCameraDirectionTests COMMAND corona_vision_camera_direction_tests)

add_executable(corona_actor_selection_routing_tests
    test_actor_selection_routing.cpp
)
target_include_directories(corona_actor_selection_routing_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/src/systems/ui
)
target_link_libraries(corona_actor_selection_routing_tests
    PRIVATE nlohmann_json::nlohmann_json
)
add_test(NAME ActorSelectionRoutingTests
         COMMAND corona_actor_selection_routing_tests)

add_executable(corona_scene_folder_tests
    test_scene_folder.cpp
    ${PROJECT_SOURCE_DIR}/src/systems/ui/cef/scene_folder.cpp
)
target_include_directories(corona_scene_folder_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/src/systems/ui
)
# scene_folder.cpp 现在会打 CFW_LOG_WARNING（缺失贴图/Assimp 扫描失败降级为警告），
# 需要 Horizon 提供日志头文件和共享的 Quill 后端。
target_link_libraries(corona_scene_folder_tests
    PRIVATE Horizon nlohmann_json::nlohmann_json bcrypt assimp::assimp
)
target_compile_definitions(corona_scene_folder_tests PRIVATE
    CORONA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
if(MSVC)
    target_compile_options(corona_scene_folder_tests PRIVATE /EHsc)
endif()
add_test(NAME SceneFolderTests COMMAND corona_scene_folder_tests)

add_executable(corona_cef_app_tests
    test_cef_app.cpp
    ${PROJECT_SOURCE_DIR}/src/shared_data_hub.cpp
)
target_include_directories(corona_cef_app_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/include
            ${PROJECT_SOURCE_DIR}/src/systems/ui
)
target_link_libraries(corona_cef_app_tests
    PRIVATE corona_ui_system corona::resource::manager
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
get_filename_component(_CORONA_CEF_TESTS_PYTHON_DIR "${Python_EXECUTABLE}" DIRECTORY)
set(_CORONA_CEF_TESTS_PATH_DIRS "${_CORONA_CEF_TESTS_PYTHON_DIR}")
foreach(_tbb_runtime IN LISTS CORONA_TBB_REDIS_DLLS)
    get_filename_component(_tbb_runtime_dir "${_tbb_runtime}" DIRECTORY)
    list(APPEND _CORONA_CEF_TESTS_PATH_DIRS "${_tbb_runtime_dir}")
endforeach()
list(REMOVE_DUPLICATES _CORONA_CEF_TESTS_PATH_DIRS)
set(_CORONA_CEF_TESTS_ENVIRONMENT)
foreach(_runtime_dir IN LISTS _CORONA_CEF_TESTS_PATH_DIRS)
    list(APPEND _CORONA_CEF_TESTS_ENVIRONMENT
         "PATH=path_list_prepend:${_runtime_dir}")
endforeach()
set_tests_properties(CefAppTests PROPERTIES
    RUN_SERIAL TRUE
    TIMEOUT 60
    ENVIRONMENT_MODIFICATION "${_CORONA_CEF_TESTS_ENVIRONMENT}"
)
unset(_CORONA_CEF_TESTS_PYTHON_DIR)
unset(_CORONA_CEF_TESTS_PATH_DIRS)
unset(_CORONA_CEF_TESTS_ENVIRONMENT)
unset(_tbb_runtime_dir)

add_executable(corona_cef_popup_overlay_tests
    test_cef_popup_overlay.cpp
    ${PROJECT_SOURCE_DIR}/src/systems/ui/cef/popup_overlay.cpp
)
target_include_directories(corona_cef_popup_overlay_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/src/systems/ui
)
add_test(NAME CefPopupOverlayTests COMMAND corona_cef_popup_overlay_tests)

# 源码级棘轮：CEF 位图的通道序必须在「纹理格式声明 / view 侧转换 / popup 侧转换」三处保持一致，
# 否则只改一边（例如换 SBGRA8_UNORM 却忘了 popup 也会转换）会让浮层颜色错乱而无测试变红。
# 本仓环境没有 pytest，因此该文件带 __main__ 入口，可直接被 ctest 执行。
add_test(NAME CefTextureChannelOrderTests
    COMMAND ${Python_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_cef_texture_channel_order.py
)

add_executable(corona_ui_surface_lifecycle_tests
    test_ui_surface_lifecycle.cpp
    ${PROJECT_SOURCE_DIR}/src/systems/ui/ui_surface_lifecycle.cpp
)
target_include_directories(corona_ui_surface_lifecycle_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/include
)
add_test(NAME UiSurfaceLifecycleTests
         COMMAND corona_ui_surface_lifecycle_tests)

add_executable(corona_camera_viewport_manager_tests
    test_camera_viewport_manager.cpp
)
target_include_directories(corona_camera_viewport_manager_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/include
)
add_test(NAME CameraViewportManagerTests
         COMMAND corona_camera_viewport_manager_tests)

add_executable(corona_vision_actor_material_bridge_tests
    test_vision_actor_material_bridge.cpp
    ${PROJECT_SOURCE_DIR}/src/systems/ui/cef/vision_actor_material_bridge.cpp
)
target_include_directories(corona_vision_actor_material_bridge_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/src/systems/ui
)
target_link_libraries(corona_vision_actor_material_bridge_tests
    PRIVATE nlohmann_json::nlohmann_json
)
add_test(NAME VisionActorMaterialBridgeTests
         COMMAND corona_vision_actor_material_bridge_tests)

add_executable(corona_vision_actor_transform_bridge_tests
    test_vision_actor_transform_bridge.cpp
    ${PROJECT_SOURCE_DIR}/src/systems/ui/cef/vision_actor_transform_bridge.cpp
  )
target_include_directories(corona_vision_actor_transform_bridge_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/src/systems/ui
)
target_link_libraries(corona_vision_actor_transform_bridge_tests
    PRIVATE nlohmann_json::nlohmann_json
)
add_test(NAME VisionActorTransformBridgeTests
         COMMAND corona_vision_actor_transform_bridge_tests)

add_executable(corona_request_response_broker_tests
    test_request_response_broker.cpp
    ${PROJECT_SOURCE_DIR}/src/systems/ui/cef/request_response_broker.cpp
)
target_include_directories(corona_request_response_broker_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/src/systems/ui/cef
)
target_link_libraries(corona_request_response_broker_tests
    PRIVATE Horizon nlohmann_json::nlohmann_json
)
add_test(NAME RequestResponseBrokerTests
         COMMAND corona_request_response_broker_tests)

add_executable(corona_viewport_gizmo_state_tests
    test_viewport_gizmo_state.cpp
    ${PROJECT_SOURCE_DIR}/src/shared_data_hub.cpp
)
target_include_directories(corona_viewport_gizmo_state_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(corona_viewport_gizmo_state_tests PRIVATE Horizon corona::resource::manager)
add_test(NAME ViewportGizmoStateTests
         COMMAND corona_viewport_gizmo_state_tests)

# 面板缩放「合并应用」策略：纯函数（无 SDL/CEF 依赖），因此不需要注入任何运行时。
add_executable(corona_panel_resize_policy_tests
    test_panel_resize_policy.cpp
    ${PROJECT_SOURCE_DIR}/src/systems/ui/panel_resize_policy.cpp
)
target_include_directories(corona_panel_resize_policy_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/include
)
add_test(NAME PanelResizePolicyTests
         COMMAND corona_panel_resize_policy_tests)

# CEF 绘制缓冲上传决策：同样是纯函数，钉住"推迟纹理重建"的语义（避免行错位斜切与空纹理帧）。
add_executable(corona_cef_paint_upload_tests
    test_cef_paint_upload.cpp
    ${PROJECT_SOURCE_DIR}/src/systems/ui/cef/cef_paint_upload.cpp
)
target_include_directories(corona_cef_paint_upload_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/include
)
add_test(NAME CefPaintUploadTests
         COMMAND corona_cef_paint_upload_tests)
