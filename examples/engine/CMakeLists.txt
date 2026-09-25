# ==============================================================================
# CoronaEngine - Main Engine Example
#
# 这是 CoronaEngine 的主要可执行程序，演示完整的引擎初始化和运行流程
# ==============================================================================

corona_add_example(
    NAME corona_engine
    SOURCES main.cpp
    COPY_ASSETS
)

# 链接 CoronaEngine 核心库（已包含所有系统）
target_link_libraries(corona_engine PRIVATE
    corona::engine
)

if(CORONA_BUILD_VISION)
    # Dynamically loaded MODULE libraries are absent from TARGET_RUNTIME_DLLS
    # of the executable. Deploy them and their dependencies explicitly.
    get_property(_vision_plugins GLOBAL PROPERTY CORONA_VISION_PLUGIN_TARGETS)
    list(APPEND _vision_plugins ocarina-backend-cuda)
    add_custom_target(corona_engine_vision_runtime
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:corona_engine>"
        DEPENDS ${_vision_plugins} copy_cuda_headers
    )
    foreach(_plugin IN LISTS _vision_plugins)
        add_custom_command(TARGET corona_engine_vision_runtime POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:${_plugin}>" $<TARGET_RUNTIME_DLLS:${_plugin}>
                "$<TARGET_FILE_DIR:corona_engine>"
            COMMAND_EXPAND_LISTS VERBATIM
        )
    endforeach()
    add_custom_command(TARGET corona_engine_vision_runtime POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "$<TARGET_FILE_DIR:ocarina-backend-cuda>/cuda"
            "$<TARGET_FILE_DIR:corona_engine>/cuda"
        VERBATIM
    )
    add_dependencies(corona_engine corona_engine_vision_runtime)
endif()

add_custom_command(TARGET corona_engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -f
            "$<TARGET_FILE_DIR:corona_engine>/cef_subprocess.exe"
    VERBATIM
)
