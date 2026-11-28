add_executable(corona_geometry_storage_snapshot_tests
    test_geometry_storage_snapshot.cpp
)
target_include_directories(corona_geometry_storage_snapshot_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/systems/geometry
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(corona_geometry_storage_snapshot_tests PRIVATE Horizon)
add_test(NAME GeometryStorageSnapshotTests
         COMMAND corona_geometry_storage_snapshot_tests)

add_executable(corona_geometry_scene_snapshot_tests test_geometry_scene_snapshot.cpp)
target_include_directories(corona_geometry_scene_snapshot_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(corona_geometry_scene_snapshot_tests PRIVATE Horizon CoronaResourceManager)
add_test(NAME GeometrySceneSnapshotTests
         COMMAND corona_geometry_scene_snapshot_tests)


add_executable(corona_model_node_state_tests test_model_node_state.cpp)
target_include_directories(corona_model_node_state_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_test(NAME GeometryModelNodeStateTests COMMAND corona_model_node_state_tests)

add_executable(corona_model_node_geometry_tests test_model_node_geometry.cpp)
target_include_directories(corona_model_node_geometry_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(corona_model_node_geometry_tests PRIVATE Horizon CoronaResourceManager)
add_test(NAME GeometryModelNodeGeometryTests COMMAND corona_model_node_geometry_tests)
