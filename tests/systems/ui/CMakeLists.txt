
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

# 窗口/呈现请求的读取规则：V8 适配器负责取值，这里只验证哪些形状合法（无需 CEF 上下文）。
add_executable(corona_request_rules_tests
    test_request_rules.cpp
)
target_include_directories(corona_request_rules_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src/systems/ui)
add_test(NAME RequestRulesTests COMMAND corona_request_rules_tests)

# 模型节点领域服务（WNC-012）：纯引擎节点数据 + JSON，无 CEF/V8/JS 路由依赖，
# 验证领域逻辑可以脱离浏览器与 JS 边界单独测试。
add_executable(corona_scene_model_node_service_tests
    test_scene_model_node_service.cpp
    ${PROJECT_SOURCE_DIR}/src/systems/ui/editor_api/scene_model_node_service.cpp
)
target_include_directories(corona_scene_model_node_service_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src/systems/ui)
target_link_libraries(corona_scene_model_node_service_tests PRIVATE nlohmann_json::nlohmann_json)
add_test(NAME SceneModelNodeServiceTests COMMAND corona_scene_model_node_service_tests)

# 环境域决策逻辑（WNC-013）：纯 DTO 校验/太阳-网格状态/物理字段提取，无 CEF 依赖。
add_executable(corona_scene_environment_service_tests
    test_scene_environment_service.cpp
    ${PROJECT_SOURCE_DIR}/src/systems/ui/editor_api/scene_environment_service.cpp
)
target_include_directories(corona_scene_environment_service_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src/systems/ui)
target_link_libraries(corona_scene_environment_service_tests PRIVATE nlohmann_json::nlohmann_json)
add_test(NAME SceneEnvironmentServiceTests COMMAND corona_scene_environment_service_tests)

# updateActor 校验（WNC-013）：空 patch/机制/相机锁/重命名规则，无 CEF 依赖。
add_executable(corona_actor_properties_service_tests
    test_actor_properties_service.cpp
    ${PROJECT_SOURCE_DIR}/src/systems/ui/editor_api/actor_properties_service.cpp
)
target_include_directories(corona_actor_properties_service_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src/systems/ui)
target_link_libraries(corona_actor_properties_service_tests PRIVATE nlohmann_json::nlohmann_json)
add_test(NAME ActorPropertiesServiceTests COMMAND corona_actor_properties_service_tests)

# Camera 渲染设置校验（WNC-013）：纯 DTO 白名单/优先级，无 CEF 依赖。
# 注意：apply_native_camera_properties 的校验暂不迁移——tests/systems/ui/test_camera_view_update.py
# 直接抽取该函数源码编译（需 g++），迁移须与 harness 更新一起做。
# 目标名保持短小：worktree 的 obj 路径接近 MAX_PATH。
add_executable(corona_camera_settings_tests
    test_camera_render_settings_service.cpp
    ${PROJECT_SOURCE_DIR}/src/systems/ui/editor_api/camera_render_settings_service.cpp
)
target_include_directories(corona_camera_settings_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src/systems/ui)
target_link_libraries(corona_camera_settings_tests PRIVATE nlohmann_json::nlohmann_json)
add_test(NAME CameraSettingsServiceTests COMMAND corona_camera_settings_tests)

# Camera 属性更新校验（WNC-013）：身份/重命名/数值范围/编辑器输入锁，无 CEF 依赖。
add_executable(corona_camera_props_tests
    test_camera_properties_service.cpp
    ${PROJECT_SOURCE_DIR}/src/systems/ui/editor_api/camera_properties_service.cpp
)
target_include_directories(corona_camera_props_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src/systems/ui)
target_link_libraries(corona_camera_props_tests PRIVATE nlohmann_json::nlohmann_json)
add_test(NAME CameraPropertiesServiceTests COMMAND corona_camera_props_tests)

# scene 投影规则（WNC-013）：身份投影/显示类型/semantic_role/phantom 物理，无 CEF 依赖。
add_executable(corona_scene_projection_tests
    test_scene_projection_service.cpp
    ${PROJECT_SOURCE_DIR}/src/systems/ui/editor_api/scene_projection_service.cpp
)
target_include_directories(corona_scene_projection_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src/systems/ui)
target_link_libraries(corona_scene_projection_tests PRIVATE nlohmann_json::nlohmann_json)
add_test(NAME SceneProjectionTests COMMAND corona_scene_projection_tests)

# 资源导入决策（WNC-013）：类型白名单/身份校验/媒体元数据映射，无 CEF 依赖。
add_executable(corona_resource_import_tests
    test_resource_import_service.cpp
    ${PROJECT_SOURCE_DIR}/src/systems/ui/editor_api/resource_import_service.cpp
)
target_include_directories(corona_resource_import_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src/systems/ui)
target_link_libraries(corona_resource_import_tests PRIVATE nlohmann_json::nlohmann_json)
add_test(NAME ResourceImportTests COMMAND corona_resource_import_tests)

# 脚本/项目文件桥规则（WNC-014）：脚本路径/项目身份/目标类型/错误映射，无 CEF 依赖。
add_executable(corona_project_script_tests
    test_project_script_service.cpp
    ${PROJECT_SOURCE_DIR}/src/systems/ui/editor_api/project_script_service.cpp
)
target_include_directories(corona_project_script_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src/systems/ui)
target_link_libraries(corona_project_script_tests PRIVATE nlohmann_json::nlohmann_json)
add_test(NAME ProjectScriptServiceTests COMMAND corona_project_script_tests)

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
add_test(NAME SceneFolderTests COMMAND ${Python_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/integration/archive_driver.py
    $<TARGET_FILE:corona_scene_folder_tests> ${CORONA_EDITOR_SOURCE_DIR})

add_executable(corona_cef_app_tests
    test_cef_app.cpp
    test_cef_lifecycle.cpp
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
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<IF:$<CONFIG:Debug>,${CEF_ROOT}/Debug/v8_context_snapshot.bin,${CEF_ROOT}/Release/v8_context_snapshot.bin>"
            "$<TARGET_FILE_DIR:corona_cef_app_tests>/v8_context_snapshot.bin"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CEF_RES_DIR}" "$<TARGET_FILE_DIR:corona_cef_app_tests>"
    VERBATIM
)
add_test(NAME CefAppTests COMMAND corona_cef_app_tests)

add_executable(corona_camera_window_identity_tests
    test_camera_window_identity.cpp
    ${PROJECT_SOURCE_DIR}/src/shared_data_hub.cpp)
target_include_directories(corona_camera_window_identity_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src/systems/ui
    ${PROJECT_SOURCE_DIR}/src/systems)
target_link_libraries(corona_camera_window_identity_tests PRIVATE corona_ui_system corona::resource::manager)
if(COMMAND helicon_install_runtime_deps)
    helicon_install_runtime_deps(corona_camera_window_identity_tests)
endif()
add_custom_command(TARGET corona_camera_window_identity_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<IF:$<CONFIG:Debug>,${CEF_ROOT}/Debug/libcef.dll,${CEF_ROOT}/Release/libcef.dll>"
        "$<TARGET_FILE_DIR:corona_camera_window_identity_tests>/libcef.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<IF:$<CONFIG:Debug>,${CEF_ROOT}/Debug/chrome_elf.dll,${CEF_ROOT}/Release/chrome_elf.dll>"
        "$<TARGET_FILE_DIR:corona_camera_window_identity_tests>/chrome_elf.dll"
    VERBATIM)
add_test(NAME CameraWindowIdentityTests COMMAND corona_camera_window_identity_tests)
set_tests_properties(CameraWindowIdentityTests PROPERTIES TIMEOUT 30)

# ScriptRuntime 具名入口的行为契约：16 条 script_scene_* 每条都真正被驱动一次，
# 断言成功形状、归档身份校验和既定的失败码；名称层面的守卫在 tests/tools 里。
add_executable(corona_script_runtime_api_tests test_script_runtime_api.cpp)
target_include_directories(corona_script_runtime_api_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/systems/ui)
target_link_libraries(corona_script_runtime_api_tests PRIVATE corona_ui_system corona::engine)
target_compile_options(corona_script_runtime_api_tests PRIVATE /EHsc)
add_test(NAME ScriptRuntimeApiTests COMMAND corona_script_runtime_api_tests)
set_tests_properties(ScriptRuntimeApiTests PROPERTIES TIMEOUT 60)

# Runs against the separately built production engine, including its real Dock
# windows. Keep the runtime explicit because the tests family omits examples.
find_program(CORONA_CEF_SMOKE_NODE NAMES node DOC "Node 22+ for the real CEF communication smoke")
string(TOLOWER "${CORONA_DEV_CONFIGURATION}" _corona_cef_configuration)
set(CORONA_CEF_SMOKE_RUNTIME
    "${PROJECT_SOURCE_DIR}/build/conan/examples/${_corona_cef_configuration}/examples/engine/$<CONFIG>"
    CACHE STRING "Production engine runtime directory for CEF communication tests")
if(CORONA_CEF_SMOKE_NODE)
    add_test(NAME CefWebCommunicationSmoke
        COMMAND "${CORONA_CEF_SMOKE_NODE}"
            "${PROJECT_SOURCE_DIR}/tests/integration/web_messages_cef_smoke.mjs"
            "${CORONA_CEF_SMOKE_RUNTIME}" --native-bindings --ctest)
    set_tests_properties(CefWebCommunicationSmoke PROPERTIES
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        RUN_SERIAL TRUE
        TIMEOUT 180
        SKIP_RETURN_CODE 77
        LABELS "cef;integration;gpu")
endif()
unset(_corona_cef_configuration)
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

add_test(NAME CefShutdownSingleProcess COMMAND corona_cef_app_tests --cef-lifecycle-test)
add_test(NAME CefShutdownEarlyClose COMMAND corona_cef_app_tests --cef-lifecycle-test --close-before-created)
foreach(_cef_lifecycle_test IN ITEMS CefShutdownSingleProcess CefShutdownEarlyClose)
    # Software rendering keeps this lifecycle regression independent of GPU load.
    # Cache roots belong to the build directory, never the user's editor profile.
    set_tests_properties(${_cef_lifecycle_test} PROPERTIES
        RUN_SERIAL TRUE
        TIMEOUT 45
        LABELS "cef;integration"
        ENVIRONMENT_MODIFICATION "${_CORONA_CEF_TESTS_ENVIRONMENT}"
        ENVIRONMENT "CORONA_CEF_ACCELERATED_OSR=software;CORONA_CEF_REMOTE_DEBUG_PORT=0;CORONA_CEF_ROOT_CACHE=${CMAKE_CURRENT_BINARY_DIR}/cef-test-cache/$<CONFIG>/${_cef_lifecycle_test}"
    )
endforeach()
unset(_cef_lifecycle_test)
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

add_executable(corona_mouse_move_dispatch_tests test_mouse_move_dispatch.cpp)
target_include_directories(corona_mouse_move_dispatch_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/include
)
add_test(NAME MouseMoveDispatchTests COMMAND corona_mouse_move_dispatch_tests)

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
# 该用例直接包含 src/systems/ui/cef/scene_tree_actor_row.h，而它现在依赖
# <corona/kernel/entity_id.h>（句柄别名），所以需要引擎 include 根。
target_include_directories(corona_scene_tree_actor_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/systems/ui
    ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(corona_scene_tree_actor_tests PRIVATE nlohmann_json::nlohmann_json)
add_test(NAME SceneTreeActorTests COMMAND corona_scene_tree_actor_tests)

# 慢原生请求判定：handler 与离屏绘制同线程，阈值边界就是"卡 UI"的判据。
# 同时断言 NativeApiRequestScope 真的把请求标记成在飞——否则卡死看门狗虽每帧轮询却永远不报。
add_executable(corona_native_api_timing_tests test_native_api_timing.cpp)
target_include_directories(corona_native_api_timing_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/systems/ui)
target_link_libraries(corona_native_api_timing_tests PRIVATE corona_ui_system)
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
add_executable(corona_imgui_draw_packet_tests
    test_imgui_draw_packet.cpp
    ${PROJECT_SOURCE_DIR}/src/systems/ui/imgui/imgui_draw_packet.cpp)
target_include_directories(corona_imgui_draw_packet_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/systems/ui)
target_link_libraries(corona_imgui_draw_packet_tests PRIVATE corona::imgui)
add_test(NAME ImGuiDrawPacketTests COMMAND corona_imgui_draw_packet_tests)

add_executable(corona_imgui_workspace_tests
    test_imgui_workspace.cpp
    ${PROJECT_SOURCE_DIR}/src/systems/ui/imgui/imgui_workspace.cpp)
target_include_directories(corona_imgui_workspace_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/systems/ui)
target_link_libraries(corona_imgui_workspace_tests PRIVATE corona::imgui)
add_test(NAME ImGuiWorkspaceTests COMMAND corona_imgui_workspace_tests)


# 变换命令合并：LatestWins 只保留同 key 的最后一条命令，若调用方把 position/rotation/scale
# 分三次入队（编辑器的 transform patch 正是如此），只剩最后一个字段生效。这个测试驱动偏离
# 真实调用方的那种写法，断言三个字段都落地。
add_executable(corona_transform_command_merge_tests test_transform_command_merge.cpp)
target_include_directories(corona_transform_command_merge_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(corona_transform_command_merge_tests PRIVATE corona::engine)
add_test(NAME TransformCommandMergeTests COMMAND corona_transform_command_merge_tests)