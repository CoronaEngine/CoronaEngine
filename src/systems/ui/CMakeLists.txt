# Create a library target for ImGui backend files
add_library(imgui_backends STATIC
        "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
)
target_include_directories(imgui_backends PUBLIC
        "${imgui_SOURCE_DIR}"
        "${imgui_SOURCE_DIR}/backends"
)
target_link_libraries(imgui_backends PUBLIC imgui SDL3::SDL3-static)


# ImGui System 配置
file(GLOB IMGUI_SYSTEM_SOURCES
    imgui_system.cpp
    vulk/vulkan_backend.cpp
    vulk/browser_manager_vulkan.cpp
    imgui/imgui_ui.cpp

    camera_viewport_manager.cpp
    viewport_gizmo_manager.cpp
    cef/browser_manager.cpp
    cef/browser_ui.cpp
    cef/cef_client.cpp
    cef/cef_query_bridge.cpp
    cef/cef_realtime_bridge.cpp
    cef/cef_py_bind.cpp
    sdl/sdl_utils.cpp
)

list(APPEND IMGUI_SYSTEM_SOURCES
    ${PROJECT_SOURCE_DIR}/include/corona/systems/ui/vulkan_backend.h
    ${PROJECT_SOURCE_DIR}/vision/src/ext/window/imgui/gizmo/ImGuizmo.cpp
)


corona_add_system(imgui
    SOURCES ${IMGUI_SYSTEM_SOURCES}
    DEPENDENCIES
        Horizon
        corona::system::network
        libcef_dll_wrapper
        debug "${CEF_LIBCEF_DEBUG}"
        optimized "${CEF_LIBCEF_RELEASE}"
        SDL3::SDL3-static
        imgui
        imgui_backends
        ws2_32
        shlwapi
        dbghelp
        psapi
        version
        winmm
        Python::Python
        nanobind-static
        corona::resource::manager
        nlohmann_json::nlohmann_json
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/cef
        ${CMAKE_SOURCE_DIR}/build_deps/imgui-src
        ${CMAKE_SOURCE_DIR}/vision/src/ext/window/imgui/gizmo
        ${CEF_INCLUDE_DIR}
        ${CEF_ROOT}
)

target_compile_definitions(corona_imgui_system PUBLIC VK_NO_PROTOTYPES)

helicon_compile_shaders(corona_imgui_system
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders"
)

target_include_directories(corona_imgui_system BEFORE PUBLIC
    "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders"
)
