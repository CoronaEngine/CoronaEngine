# ==============================================================================
# Script System (with Python binding)
# ==============================================================================

find_package(nanobind CONFIG REQUIRED)

# Build nanobind static library (required for Python binding).
if(NOT TARGET nanobind-static)
    nanobind_build_library(nanobind-static)
endif()

add_library(CoronaPythonRuntimeCoordinator STATIC
    python/python_runtime_coordinator.cpp
    ${PROJECT_SOURCE_DIR}/include/corona/systems/script/python_runtime_coordinator.h
)
add_library(corona::python::runtime ALIAS CoronaPythonRuntimeCoordinator)
target_include_directories(CoronaPythonRuntimeCoordinator PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)
target_compile_features(CoronaPythonRuntimeCoordinator PUBLIC cxx_std_20)

# Create system library
corona_add_system(script
    SOURCES
        ${PROJECT_SOURCE_DIR}/include/corona/systems/script/python_lifecycle.h
        ${PROJECT_SOURCE_DIR}/include/corona/systems/script/python_runtime_coordinator.h
        script_system.cpp
        ${PROJECT_SOURCE_DIR}/src/engine/engine_runtime_api.cpp
        python/corona_engine_module.cpp
        python/engine_bindings.cpp
        python/editor_host_bindings.cpp
        python/python_api.cpp
        python/python_path_config.cpp
        python/python_error_handler.cpp
        camera_follow_controller.cpp
    DEPENDENCIES
        Python::Python
        nanobind-static
        corona::resource::manager
        Horizon
        SDL3::SDL3-static
        corona::python::runtime
        # geometry：复用 build_mesh_devices_from_scene（GPU MeshDevice 构建单一来源）。
        # geometry 不依赖 script，此依赖无环。
        corona::system::geometry
)

# Add Python-specific include directories
target_include_directories(corona_script_system PUBLIC
    ${Python_INCLUDE_DIRS}
)

target_include_directories(corona_script_system PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Add Python library directories
target_link_directories(corona_script_system PUBLIC
    ${Python_LIBRARY_DIRS}
)

if(BUILD_CORONA_TESTING OR BUILD_TESTING)
    add_executable(corona_python_lifecycle_tests
        tests/test_python_lifecycle.cpp
    )
    target_include_directories(corona_python_lifecycle_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/include
    )
    add_test(NAME PythonLifecycleTests COMMAND corona_python_lifecycle_tests)

    add_executable(corona_python_runtime_coordinator_tests
        tests/test_python_runtime_coordinator.cpp
    )
    target_include_directories(corona_python_runtime_coordinator_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/include
    )
    target_link_libraries(corona_python_runtime_coordinator_tests PRIVATE
        corona::python::runtime
    )
    add_test(NAME PythonRuntimeCoordinatorTests COMMAND corona_python_runtime_coordinator_tests)
endif()

