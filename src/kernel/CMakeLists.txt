set(CORONA_ENGINE_KERNEL_SOURCE_FILES
    core/kernel_context.cpp
    event/event_bus.cpp
    event/event_stream.cpp
    system/system_base.cpp
    system/system_manager.cpp
)

set(CORONA_ENGINE_KERNEL_HEADER_FILES
    ${PROJECT_SOURCE_DIR}/include/corona/kernel/core/kernel_context.h
    ${PROJECT_SOURCE_DIR}/include/corona/kernel/event/event_concepts.h
    ${PROJECT_SOURCE_DIR}/include/corona/kernel/event/i_event_bus.h
    ${PROJECT_SOURCE_DIR}/include/corona/kernel/event/i_event_stream.h
    ${PROJECT_SOURCE_DIR}/include/corona/kernel/system/i_system.h
    ${PROJECT_SOURCE_DIR}/include/corona/kernel/system/i_system_context.h
    ${PROJECT_SOURCE_DIR}/include/corona/kernel/system/i_system_manager.h
    ${PROJECT_SOURCE_DIR}/include/corona/kernel/system/system_base.h
)

add_library(CoronaEngineKernel STATIC
    ${CORONA_ENGINE_KERNEL_SOURCE_FILES}
    ${CORONA_ENGINE_KERNEL_HEADER_FILES}
)

add_library(corona::engine::kernel ALIAS CoronaEngineKernel)

target_include_directories(CoronaEngineKernel
    PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(CoronaEngineKernel
    PUBLIC
    Horizon
)

target_compile_features(CoronaEngineKernel PUBLIC cxx_std_20)

if(BUILD_CORONA_TESTING OR BUILD_TESTING)
    add_executable(corona_system_stop_timeout_tests
        tests/test_system_stop_timeout.cpp
    )
    target_link_libraries(corona_system_stop_timeout_tests PRIVATE CoronaEngineKernel)
    add_test(NAME SystemStopTimeoutTests
             COMMAND corona_system_stop_timeout_tests)

    add_executable(corona_system_manager_lifecycle_tests
        tests/test_system_manager_lifecycle.cpp
    )
    target_link_libraries(corona_system_manager_lifecycle_tests PRIVATE CoronaEngineKernel)
    add_test(NAME SystemManagerLifecycleTests
             COMMAND corona_system_manager_lifecycle_tests)
endif()
