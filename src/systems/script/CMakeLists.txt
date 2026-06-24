# ==============================================================================
# Script System (with Python binding)
# ==============================================================================

# Build nanobind static library (required for Python binding)
if(DEFINED nanobind_SOURCE_DIR)
    include(${nanobind_SOURCE_DIR}/cmake/nanobind-config.cmake)
    nanobind_build_library(nanobind-static)
else()
    message(FATAL_ERROR "[CMake] nanobind source directory not found")
endif()

# Create system library
corona_add_system(script
    SOURCES
        script_system.cpp
        python/corona_engine_api.cpp
        python/corona_engine_module.cpp
        python/engine_bindings.cpp
        python/python_api.cpp
        python/python_hotfix.cpp
        python/python_path_config.cpp
        python/python_error_handler.cpp
        camera_follow_controller.cpp
    DEPENDENCIES
        Python::Python
        nanobind-static
        corona::resource::manager
        Horizon
        SDL3::SDL3-static
)

# Add Python-specific include directories
target_include_directories(corona_script_system PUBLIC
    ${Python_INCLUDE_DIRS}
    ${nanobind_SOURCE_DIR}/include
)

target_include_directories(corona_script_system PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Add Python library directories
target_link_directories(corona_script_system PUBLIC
    ${Python_LIBRARY_DIRS}
)

