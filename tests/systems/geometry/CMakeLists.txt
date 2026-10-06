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
