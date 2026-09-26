# ==============================================================================
# Geometry System
# ==============================================================================

corona_add_system(geometry
    SOURCES
        geometry_system.cpp
        geometry_queries.cpp
        geometry_lod.cpp
        geometry_internal.h
        geometry_mesh_builder.cpp
        frustum.cpp
        actor_cache.cpp
    DEPENDENCIES
        Horizon
        corona::resource::manager
        nlohmann_json::nlohmann_json
)

if(BUILD_CORONA_TESTING OR BUILD_TESTING)
    add_executable(corona_geometry_storage_snapshot_tests
        tests/test_geometry_storage_snapshot.cpp
    )
    target_include_directories(corona_geometry_storage_snapshot_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(corona_geometry_storage_snapshot_tests PRIVATE Horizon)
    add_test(NAME GeometryStorageSnapshotTests
             COMMAND corona_geometry_storage_snapshot_tests)

    add_executable(corona_shadow_lod_state_tests tests/test_shadow_lod_state.cpp)
    target_include_directories(corona_shadow_lod_state_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME GeometryShadowLodStateTests COMMAND corona_shadow_lod_state_tests)
endif()
