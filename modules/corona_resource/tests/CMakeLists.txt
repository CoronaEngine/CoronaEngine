# CoronaResource Tests

# Test executable
add_executable(corona_resource_tests
    test_resource_manager.cpp
)

target_link_libraries(corona_resource_tests
    PRIVATE
        corona::resource::manager
)

target_include_directories(corona_resource_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

corona_copy_runtime_files(gf corona_resource_tests)
corona_copy_runtime_files(assimp::assimp corona_resource_tests)

# Add test
add_test(NAME ResourceManagerTests COMMAND corona_resource_tests)

# ----------------------------------------------------------------------------
# 骨骼动画运行时求值单测（P1）：纯 CPU 数学自洽性，无需 GPU / 资源文件
# ----------------------------------------------------------------------------
add_executable(corona_animation_pose_tests
    test_animation_pose.cpp
)

target_link_libraries(corona_animation_pose_tests
    PRIVATE
        corona::resource::manager
)

target_include_directories(corona_animation_pose_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)


add_test(NAME AnimationPoseTests COMMAND corona_animation_pose_tests)

# ----------------------------------------------------------------------------
# Mesh import safety tests: CPU-side guards for uint16 index conversion.
# ----------------------------------------------------------------------------
add_executable(corona_mesh_index_tests
    test_mesh_index_validation.cpp
)

target_link_libraries(corona_mesh_index_tests
    PRIVATE
        corona::resource::manager
        meshoptimizer
)

target_include_directories(corona_mesh_index_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)


add_test(NAME MeshIndexValidationTests COMMAND corona_mesh_index_tests)

