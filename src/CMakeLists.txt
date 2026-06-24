# ==============================================================================
# CoronaEngine - Source Library
#
# 构建引擎核心库和系统模块
# ==============================================================================

# ------------------------------------------------------------------------------
# 自动发现并添加系统模块
# ------------------------------------------------------------------------------
add_subdirectory(kernel)

message(STATUS "[CoronaEngine] Discovering system modules...")

file(GLOB _CORONA_SYSTEM_DIRS LIST_DIRECTORIES true
        "${CMAKE_CURRENT_SOURCE_DIR}/systems/*")

foreach (_system_dir IN LISTS _CORONA_SYSTEM_DIRS)
    if (IS_DIRECTORY "${_system_dir}")
        get_filename_component(_system_name "${_system_dir}" NAME)

        if (EXISTS "${_system_dir}/CMakeLists.txt")
            add_subdirectory("systems/${_system_name}")
        else ()
            message(WARNING "[CoronaEngine] System '${_system_name}' has no CMakeLists.txt, skipping")
        endif ()
    endif ()
endforeach ()

unset(_CORONA_SYSTEM_DIRS)
unset(_system_dir)
unset(_system_name)


# ==============================================================================
# CoronaEngine 核心库
# ==============================================================================
add_library(CoronaEngine STATIC
        engine.cpp
        shared_data_hub.cpp
        ${PROJECT_SOURCE_DIR}/include/corona/engine.h
        ${PROJECT_SOURCE_DIR}/include/corona/shared_data_hub.h
        ${PROJECT_SOURCE_DIR}/include/corona/events/acoustics_system_events.h
        ${PROJECT_SOURCE_DIR}/include/corona/events/display_system_events.h
        ${PROJECT_SOURCE_DIR}/include/corona/events/engine_events.h
        ${PROJECT_SOURCE_DIR}/include/corona/events/geometry_system_events.h
        ${PROJECT_SOURCE_DIR}/include/corona/events/mechanics_system_events.h
        ${PROJECT_SOURCE_DIR}/include/corona/events/network_system_events.h
        ${PROJECT_SOURCE_DIR}/include/corona/events/optics_system_events.h
        ${PROJECT_SOURCE_DIR}/include/corona/events/script_system_events.h
        ${PROJECT_SOURCE_DIR}/include/corona/events/imgui_system_events.h
)

add_library(corona::engine ALIAS CoronaEngine)

# 设置包含目录
target_include_directories(CoronaEngine PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

# Public system headers include generated shader headers through relative GLSL()
# paths. Keep the generated roots ahead of transitive dependency include dirs so
# stale build-cache shader headers cannot shadow the current Helicon output.
target_include_directories(CoronaEngine BEFORE PUBLIC
        ${CMAKE_CURRENT_BINARY_DIR}/systems/optics/generated_shaders
        ${CMAKE_CURRENT_BINARY_DIR}/systems/display/generated_shaders
        ${CMAKE_CURRENT_BINARY_DIR}/systems/ui/generated_shaders
)

# 链接依赖
target_link_libraries(CoronaEngine PUBLIC
        corona::engine::kernel
        corona::system::acoustics
        corona::system::optics
        corona::system::mechanics
        corona::system::geometry
        corona::system::display
        corona::system::network
        corona::system::script
        corona::system::imgui
)

# 设置 C++ 标准
target_compile_features(CoronaEngine PUBLIC cxx_std_20)

# 配置运行时依赖（Python DLLs/PDBs 等）
# 这会将依赖文件列表存储在 CoronaEngine 目标的 INTERFACE_CORONA_RUNTIME_DEPS 属性中
# 供消费者（如 examples）通过 corona_install_runtime_deps() 安装
corona_configure_runtime_deps(CoronaEngine)

# 配置编辑器资源
if (BUILD_CORONA_EDITOR)
    corona_configure_corona_editor(CoronaEngine)
endif ()

message(STATUS "[CoronaEngine] Core engine library configured")
