add_executable(corona_vision_camera_direction_tests
    test_vision_camera_direction.cpp
)

add_executable(corona_ik_chain_config_tests test_ik_chain_config.cpp)
target_include_directories(corona_ik_chain_config_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/systems/ui)
target_link_libraries(corona_ik_chain_config_tests PRIVATE nlohmann_json::nlohmann_json corona::resource::manager)
add_test(NAME IkChainConfigTests COMMAND corona_ik_chain_config_tests)
add_executable(corona_model_node_config_tests test_model_node_config.cpp)
# model_node_config.h 现在包含 <corona/systems/geometry/model_node_state.h>，
# 因此需要 include/ 而不只是 UI 的私有头目录。
target_include_directories(corona_model_node_config_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src/systems/ui)
target_link_libraries(corona_model_node_config_tests PRIVATE nlohmann_json::nlohmann_json)
add_test(NAME ModelNodeConfigTests COMMAND corona_model_node_config_tests)
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

add_executable(corona_model_node_page_tests test_model_node_page.cpp)
target_include_directories(corona_model_node_page_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/systems/ui)
add_test(NAME ModelNodePageTests COMMAND corona_model_node_page_tests)

# 场景树行契约：钉住 UI 依赖的字段，防止快照重新退回 actor_to_json 的整份派发数据。
add_executable(corona_scene_tree_actor_tests test_scene_tree_actor.cpp)
target_include_directories(corona_scene_tree_actor_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/systems/ui)
target_link_libraries(corona_scene_tree_actor_tests PRIVATE nlohmann_json::nlohmann_json)
add_test(NAME SceneTreeActorTests COMMAND corona_scene_tree_actor_tests)

# 慢原生请求判定：handler 与离屏绘制同线程，阈值边界就是"卡 UI"的判据。
add_executable(corona_native_api_timing_tests test_native_api_timing.cpp)
target_include_directories(corona_native_api_timing_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/systems/ui)
add_test(NAME NativeApiTimingTests COMMAND corona_native_api_timing_tests)

# 资源缓存"尽力而为读"契约：UI 线程读资源不得等待加载线程持有的条目锁。
# 放在 UI 测试树里，因为被钉住的正是"绘制/输入线程不得无界等待"这条规则。
add_executable(corona_resource_cache_nowait_tests test_resource_cache_nowait.cpp)
target_link_libraries(corona_resource_cache_nowait_tests PRIVATE corona::resource::manager)
corona_copy_tbb_runtime_artifacts(corona_resource_cache_nowait_tests)
add_test(NAME ResourceCacheNowaitTests COMMAND corona_resource_cache_nowait_tests)

# 编辑器状态派发契约：所有 handler 只在 owner（CEF UI）线程执行，跨线程调用必须
# marshal 且有界等待；唯一的两条例外是 owner 正阻塞等 Python（就地执行，交接协议）
# 和还没有 owner 线程（CEF 未起，加锁就地执行）。纯头文件实现，不需要 CEF 运行时。
add_executable(corona_editor_dispatch_tests test_editor_dispatch.cpp)
target_include_directories(corona_editor_dispatch_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/src/systems/ui
)
add_test(NAME EditorDispatchTests COMMAND corona_editor_dispatch_tests)
