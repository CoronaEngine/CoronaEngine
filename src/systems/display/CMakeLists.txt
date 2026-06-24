# ==============================================================================
# Display System
# ==============================================================================

corona_add_system(display
    SOURCES
        display_system.cpp
    DEPENDENCIES
        Horizon
)

helicon_compile_shaders(corona_display_system
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders"
)

target_include_directories(corona_display_system BEFORE PUBLIC
    "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders"
)
