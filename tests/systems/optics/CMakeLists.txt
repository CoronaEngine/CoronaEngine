if(CORONA_BUILD_VISION)
    add_executable(corona_switch_profile_scope_tests test_switch_profile_scope.cpp)
    target_include_directories(corona_switch_profile_scope_tests PRIVATE ${CMAKE_SOURCE_DIR}/vision/src)
    target_link_libraries(corona_switch_profile_scope_tests PRIVATE ocarina-include)
    add_test(NAME VisionSwitchProfileScopeTests
        COMMAND ${Python_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/tests/check_switch_profile_scope.py
                $<TARGET_FILE:corona_switch_profile_scope_tests>
                ${CMAKE_CURRENT_BINARY_DIR}/switch-profile-scope)
    set_tests_properties(VisionSwitchProfileScopeTests PROPERTIES TIMEOUT 30)

    add_executable(corona_vision_model_normalization_tests
        test_vision_model_normalization.cpp
        ${CMAKE_SOURCE_DIR}/vision/src/render_core/shape/model_normalization.cpp
    )
    target_include_directories(corona_vision_model_normalization_tests
        PRIVATE
            ${CMAKE_SOURCE_DIR}/vision/src
    )
    target_link_libraries(corona_vision_model_normalization_tests
        PRIVATE vision-all_libs
    )
    add_test(NAME VisionModelNormalizationTests
             COMMAND corona_vision_model_normalization_tests)

    add_executable(corona_vision_render_mode_config_tests
        test_vision_render_mode_config.cpp
    )
    target_include_directories(corona_vision_render_mode_config_tests
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${PROJECT_SOURCE_DIR}/src/systems/optics
            ${CMAKE_SOURCE_DIR}/vision/src
    )
    target_link_libraries(corona_vision_render_mode_config_tests
        PRIVATE
            corona::system::optics
            vision-all_libs
    )
    add_test(NAME VisionRenderModeConfigTests
             COMMAND corona_vision_render_mode_config_tests)
    set_tests_properties(VisionRenderModeConfigTests PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:vision-material-principled_bsdf>")

    add_executable(corona_lightfield_geometry_tests
        test_lightfield_geometry.cpp
    )
    target_include_directories(corona_lightfield_geometry_tests
        PRIVATE ${CMAKE_SOURCE_DIR}/vision/src
    )
    target_link_libraries(corona_lightfield_geometry_tests
        PRIVATE vision-all_libs
    )
    add_test(NAME LightFieldGeometryTests
             COMMAND corona_lightfield_geometry_tests)
    set_tests_properties(LightFieldGeometryTests PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:vision-material-principled_bsdf>")

    add_executable(corona_vision_material_adapter_tests
        test_vision_material_adapter.cpp
    )
    target_include_directories(corona_vision_material_adapter_tests
        PRIVATE
            ${PROJECT_SOURCE_DIR}/src/systems/optics
            ${CMAKE_SOURCE_DIR}/vision/src
    )
    target_link_libraries(corona_vision_material_adapter_tests
        PRIVATE
            corona::system::optics
            vision-all_libs
    )
    add_test(NAME VisionMaterialAdapterTests COMMAND corona_vision_material_adapter_tests)
    set_tests_properties(VisionMaterialAdapterTests PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:vision-material-principled_bsdf>"
    )

    add_executable(corona_vision_pipeline_key_tests
        test_vision_pipeline_key.cpp
    )
    target_include_directories(corona_vision_pipeline_key_tests
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )
    target_link_libraries(corona_vision_pipeline_key_tests
        PRIVATE
            corona::system::optics
    )
    add_test(NAME VisionPipelineKeyTests
             COMMAND corona_vision_pipeline_key_tests)
    set_tests_properties(VisionPipelineKeyTests PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:vision-material-principled_bsdf>")

    add_executable(corona_vision_scene_resource_tests
        test_vision_scene_resource.cpp
    )
    target_include_directories(corona_vision_scene_resource_tests
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )
    target_link_libraries(corona_vision_scene_resource_tests
        PRIVATE
            corona::system::optics
    )
    add_test(NAME VisionSceneResourceTests
             COMMAND corona_vision_scene_resource_tests)
    set_tests_properties(VisionSceneResourceTests PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:vision-material-principled_bsdf>")

    add_executable(corona_vision_interop_lifetime_tests
        test_vision_interop_lifetime.cpp
    )
    target_include_directories(corona_vision_interop_lifetime_tests
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${PROJECT_SOURCE_DIR}/src/systems/optics
    )
    target_link_libraries(corona_vision_interop_lifetime_tests
        PRIVATE
            corona::system::optics
    )
    add_test(NAME VisionInteropLifetimeTests
             COMMAND corona_vision_interop_lifetime_tests)
    set_tests_properties(VisionInteropLifetimeTests PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:vision-material-principled_bsdf>")

    add_executable(corona_vision_geometry_gpu_resource_tests
        test_vision_geometry_gpu_resource.cpp
    )
    target_include_directories(corona_vision_geometry_gpu_resource_tests
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${PROJECT_SOURCE_DIR}/src/systems/optics
            ${CMAKE_SOURCE_DIR}/vision/src
    )
    target_link_libraries(corona_vision_geometry_gpu_resource_tests
        PRIVATE
            corona::system::optics
            vision-all_libs
    )
    add_test(NAME VisionGeometryGpuResourceTests
             COMMAND corona_vision_geometry_gpu_resource_tests)
    add_test(NAME VisionWorldBoundsGpuTests
             COMMAND corona_vision_geometry_gpu_resource_tests --world-bounds-gpu)
    set_tests_properties(VisionGeometryGpuResourceTests VisionWorldBoundsGpuTests PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:vision-material-principled_bsdf>")
endif()

add_executable(corona_optics_actor_pick_readback_worker_tests
    test_actor_pick_readback_worker.cpp
    ${PROJECT_SOURCE_DIR}/src/systems/optics/actor_pick_readback_worker.cpp
)
target_include_directories(corona_optics_actor_pick_readback_worker_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src/systems/optics
)
add_test(NAME OpticsActorPickReadbackWorkerTests
         COMMAND corona_optics_actor_pick_readback_worker_tests)

add_executable(corona_optics_native_frame_throttle_tests
    test_native_frame_throttle.cpp
)
target_include_directories(corona_optics_native_frame_throttle_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/systems/optics
)
target_link_libraries(corona_optics_native_frame_throttle_tests PRIVATE Horizon)
add_test(NAME OpticsNativeFrameThrottleTests
         COMMAND corona_optics_native_frame_throttle_tests)

add_executable(corona_optics_storage_snapshot_tests
    test_storage_snapshot.cpp
)
target_include_directories(corona_optics_storage_snapshot_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/systems/optics
)
target_link_libraries(corona_optics_storage_snapshot_tests PRIVATE Horizon)
add_test(NAME OpticsStorageSnapshotTests
         COMMAND corona_optics_storage_snapshot_tests)

add_executable(corona_optics_debug_label_tests
    test_optics_debug_labels.cpp
)
target_include_directories(corona_optics_debug_label_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/systems/optics
)
add_test(NAME OpticsDebugLabelTests
         COMMAND corona_optics_debug_label_tests)

add_executable(corona_native_diagnostics_tests
    test_native_diagnostics.cpp
)
target_include_directories(corona_native_diagnostics_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/systems/optics
        ${PROJECT_SOURCE_DIR}/src/systems/optics/..
)
add_test(NAME NativeDiagnosticsTests
         COMMAND corona_native_diagnostics_tests)

add_executable(corona_shadow_culling_tests
    test_shadow_culling.cpp
    ${CMAKE_SOURCE_DIR}/src/systems/geometry/frustum.cpp
)
target_include_directories(corona_shadow_culling_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src/systems/optics
)
target_link_libraries(corona_shadow_culling_tests PRIVATE Horizon corona::resource::manager)
add_test(NAME ShadowCullingTests
         COMMAND corona_shadow_culling_tests)

add_executable(corona_viewport_gizmo_math_tests
    test_viewport_gizmo_math.cpp
)
target_include_directories(corona_viewport_gizmo_math_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src/systems/optics
)
target_link_libraries(corona_viewport_gizmo_math_tests PRIVATE Horizon)
add_test(NAME ViewportGizmoMathTests
         COMMAND corona_viewport_gizmo_math_tests)
