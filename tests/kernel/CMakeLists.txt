add_executable(corona_system_stop_timeout_tests
    test_system_stop_timeout.cpp
)
target_link_libraries(corona_system_stop_timeout_tests PRIVATE CoronaEngineKernel)
add_test(NAME SystemStopTimeoutTests
         COMMAND corona_system_stop_timeout_tests)

add_executable(corona_system_manager_lifecycle_tests
    test_system_manager_lifecycle.cpp
)
target_link_libraries(corona_system_manager_lifecycle_tests PRIVATE CoronaEngineKernel)
add_test(NAME SystemManagerLifecycleTests
         COMMAND corona_system_manager_lifecycle_tests)
