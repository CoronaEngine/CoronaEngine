add_executable(corona_geometry_storage_snapshot_tests
    test_geometry_storage_snapshot.cpp
)
target_include_directories(corona_geometry_storage_snapshot_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/systems/geometry
)
target_link_libraries(corona_geometry_storage_snapshot_tests PRIVATE Horizon)
add_test(NAME GeometryStorageSnapshotTests
         COMMAND corona_geometry_storage_snapshot_tests)

add_executable(corona_shadow_lod_state_tests test_shadow_lod_state.cpp)
target_include_directories(corona_shadow_lod_state_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/systems/geometry)
add_test(NAME GeometryShadowLodStateTests COMMAND corona_shadow_lod_state_tests)

add_executable(corona_model_node_state_tests test_model_node_state.cpp)
target_include_directories(corona_model_node_state_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_test(NAME GeometryModelNodeStateTests COMMAND corona_model_node_state_tests)

add_executable(corona_model_node_pick_tests test_model_node_pick.cpp)
# test_model_node_pick.cpp 直接包含 <corona/engine/engine_runtime_api.h>，
# 所以需要 include/ 而不只是 optics 的私有头目录。
target_include_directories(corona_model_node_pick_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src/systems/optics)
add_test(NAME GeometryModelNodePickTests COMMAND corona_model_node_pick_tests)
add_executable(corona_model_node_geometry_tests test_model_node_geometry.cpp)
target_include_directories(corona_model_node_geometry_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(corona_model_node_geometry_tests PRIVATE Horizon CoronaResourceManager)
add_test(NAME GeometryModelNodeGeometryTests COMMAND corona_model_node_geometry_tests)

add_executable(corona_native_render_load_guard_tests test_native_render_load_guard.cpp)
target_include_directories(corona_native_render_load_guard_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_test(NAME GeometryNativeRenderLoadGuardTests COMMAND corona_native_render_load_guard_tests)
