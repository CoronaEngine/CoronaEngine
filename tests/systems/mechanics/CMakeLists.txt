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
