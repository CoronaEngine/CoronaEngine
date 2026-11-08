add_executable(corona_ik_runtime_tests test_ik_runtime.cpp)
target_include_directories(corona_ik_runtime_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/systems/mechanics)
target_link_libraries(corona_ik_runtime_tests PRIVATE corona::resource::manager)
corona_copy_tbb_runtime_artifacts(corona_ik_runtime_tests)
add_test(NAME IkRuntimeTests COMMAND corona_ik_runtime_tests)

add_executable(corona_ik_ground_query_tests test_ik_ground_query.cpp)
target_include_directories(corona_ik_ground_query_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src/systems/mechanics)
target_link_libraries(corona_ik_ground_query_tests PRIVATE Horizon corona::resource::manager)
corona_copy_tbb_runtime_artifacts(corona_ik_ground_query_tests)
add_test(NAME IkGroundQueryTests COMMAND corona_ik_ground_query_tests)

add_executable(corona_prepared_compute_lifetime_tests test_prepared_compute_lifetime.cpp
    ${PROJECT_SOURCE_DIR}/src/systems/mechanics/gpu_skinning.cpp)
target_include_directories(corona_prepared_compute_lifetime_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/systems/mechanics)
target_link_libraries(corona_prepared_compute_lifetime_tests PRIVATE Horizon corona::resource::manager)
if(COMMAND helicon_install_runtime_deps)
    helicon_install_runtime_deps(corona_prepared_compute_lifetime_tests)
endif()
horizon_install_vulkan_runtime(corona_prepared_compute_lifetime_tests)
add_test(NAME PreparedComputeLifetimeTests COMMAND corona_prepared_compute_lifetime_tests)
set_tests_properties(PreparedComputeLifetimeTests PROPERTIES RUN_SERIAL TRUE TIMEOUT 90)
