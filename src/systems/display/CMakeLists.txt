# ==============================================================================
# Display System
# ==============================================================================

corona_add_system(display
    SOURCES
        display_system.cpp
        presentation_surface.cpp
    DEPENDENCIES
        Horizon
        corona::resource::manager
)

# The fixed Horizon revision exposes device/instance access through this
# internal header. Keep that dependency private to the native surface adapter.
target_include_directories(corona_display_system PRIVATE "${PROJECT_SOURCE_DIR}/Horizon/src")
if(APPLE)
    set_source_files_properties(presentation_surface.cpp PROPERTIES LANGUAGE OBJCXX)
    set_target_properties(corona_display_system PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES)
    target_link_libraries(corona_display_system PRIVATE "-framework AppKit" "-framework QuartzCore")
endif()

helicon_compile_shaders(corona_display_system
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders"
)
