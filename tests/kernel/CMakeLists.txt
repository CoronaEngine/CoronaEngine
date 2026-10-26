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

# 帧图（DAG 调度器）契约测试：拓扑序、环检测、速率门控、子树关闭、异常中止、统计。
add_executable(corona_frame_graph_tests
    test_frame_graph.cpp
)
target_link_libraries(corona_frame_graph_tests PRIVATE CoronaEngineKernel)
add_test(NAME FrameGraphTests
         COMMAND corona_frame_graph_tests)

# 单线程 DAG 执行器：依赖装配、执行序、帧时间注入、速率门控、异常隔离、线程生命周期。
add_executable(corona_systems_thread_tests
    test_systems_thread.cpp
)
target_link_libraries(corona_systems_thread_tests PRIVATE CoronaEngineKernel)
add_test(NAME SystemsThreadTests
         COMMAND corona_systems_thread_tests)
