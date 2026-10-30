# ==============================================================================
# corona_add_system.cmake
#
# Purpose:
#   标准化系统模块的构建流程，减少重复的样板代码
#
# Usage:
#   corona_add_system(<system_name>
#       SOURCES <source_files...>
#       [NAMESPACE <namespace>]
#    [DEPENDENCIES <deps...>]
#       [INCLUDE_DIRS <dirs...>]
# )
#
# Example:
#   corona_add_system(display
#       SOURCES display_system.cpp
#   )
#
#   corona_add_system(mechanics
#       SOURCES mechanics_system.cpp rigid_body.cpp
#   DEPENDENCIES corona::physics::core
#   )
#
# Behavior:
#   - Creates a static library target named `corona_<system_name>_system`
#   - Creates an alias `corona::<namespace>::<system_name>` (default namespace: "system")
#   - Automatically sets up include directories and links to corona::engine::kernel
#   - Applies consistent compile features (C++20)
# ==============================================================================

include_guard(GLOBAL)

function(corona_add_system SYSTEM_NAME)
    set(options QUIET)
    set(oneValueArgs NAMESPACE)
    set(multiValueArgs SOURCES DEPENDENCIES INCLUDE_DIRS)
    cmake_parse_arguments(SYS "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    # 验证必需参数
    if(NOT SYSTEM_NAME)
        message(FATAL_ERROR "[corona_add_system] SYSTEM_NAME is required")
    endif()

    if(NOT SYS_SOURCES)
        message(FATAL_ERROR "[corona_add_system] SOURCES is required for system '${SYSTEM_NAME}'")
    endif()

    # 默认命名空间为 "system"
    if(NOT SYS_NAMESPACE)
        set(SYS_NAMESPACE "system")
    endif()

    # 目标名称规范: corona_<name>_system
    set(TARGET_NAME "corona_${SYSTEM_NAME}_system")

    # 收集所有源文件和头文件（包括对应的头文件）
    set(ALL_FILES ${SYS_SOURCES})

    # 查找对应的头文件
    set(HEADER_DIR "${PROJECT_SOURCE_DIR}/include/corona/systems")
    if(EXISTS "${HEADER_DIR}/${SYSTEM_NAME}/${SYSTEM_NAME}_system.h")
        list(APPEND ALL_FILES "${HEADER_DIR}/${SYSTEM_NAME}/${SYSTEM_NAME}_system.h")
    endif()

    # 创建静态库目标
    add_library(${TARGET_NAME} STATIC ${ALL_FILES})

    # 创建带命名空间的别名: corona::<namespace>::<name>
    add_library(corona::${SYS_NAMESPACE}::${SYSTEM_NAME} ALIAS ${TARGET_NAME})

    # 设置标准包含目录，所有系统都共享统一的公共头文件
    target_include_directories(${TARGET_NAME} PUBLIC
        ${PROJECT_SOURCE_DIR}/include
    )

    # 添加额外的包含目录（如果有）
    if(SYS_INCLUDE_DIRS)
        target_include_directories(${TARGET_NAME} PUBLIC ${SYS_INCLUDE_DIRS})
    endif()

    # 链接到核心框架（所有系统的基础依赖）
    target_link_libraries(${TARGET_NAME} PUBLIC corona::engine::kernel)

    # 链接额外的依赖（如果有）
    if(SYS_DEPENDENCIES)
        target_link_libraries(${TARGET_NAME} PUBLIC ${SYS_DEPENDENCIES})
    endif()

    # 设置 C++ 标准
    target_compile_features(${TARGET_NAME} PUBLIC cxx_std_20)

    # 输出状态信息（除非设置了 QUIET）
    if(NOT SYS_QUIET)
        message(STATUS "[System] ${SYSTEM_NAME} system configured (target: ${TARGET_NAME})")
    endif()
endfunction()
