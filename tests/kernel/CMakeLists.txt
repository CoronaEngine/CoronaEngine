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

# Lock timeout diagnostics: the same header compiled with the diagnostics switch on
# must fail loudly (CRITICAL log + stack + throw) instead of waiting forever. The
# definition is PRIVATE here on purpose: this target only exercises header-inline
# Storage code, mirroring Horizon's own public-header test, while the engine build
# keeps the default (off) setting.
add_executable(corona_storage_lock_timeout_tests
    test_storage_lock_timeout.cpp
)
target_compile_definitions(corona_storage_lock_timeout_tests PRIVATE
    CFW_ENABLE_LOCK_TIMEOUT=1
    CFW_LOCK_TIMEOUT_MS=300
)
target_link_libraries(corona_storage_lock_timeout_tests PRIVATE Horizon)
add_test(NAME StorageLockTimeoutTests
         COMMAND corona_storage_lock_timeout_tests)
