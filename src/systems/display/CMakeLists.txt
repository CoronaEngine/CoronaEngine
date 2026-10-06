# ==============================================================================
# Display System
# ==============================================================================

corona_add_system(display
    SOURCES
        display_system.cpp
    DEPENDENCIES
        Horizon
        corona::resource::manager
)

helicon_compile_shaders(corona_display_system
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders"
)
