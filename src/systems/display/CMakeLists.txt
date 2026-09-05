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

if(BUILD_CORONA_TESTING OR BUILD_TESTING)
    add_executable(corona_ui_surface_removal_race_tests
        tests/test_ui_surface_removal_race.cpp
        ${PROJECT_SOURCE_DIR}/src/shared_data_hub.cpp
        ${PROJECT_SOURCE_DIR}/src/systems/ui/ui_surface_lifecycle.cpp
    )
    target_include_directories(corona_ui_surface_removal_race_tests
        PRIVATE ${PROJECT_SOURCE_DIR}/include
    )
    target_link_libraries(corona_ui_surface_removal_race_tests
        PRIVATE corona_display_system corona::resource::manager
    )
    if(COMMAND helicon_install_runtime_deps)
        helicon_install_runtime_deps(corona_ui_surface_removal_race_tests)
    endif()
    add_test(NAME UiSurfaceRemovalRaceTests
             COMMAND corona_ui_surface_removal_race_tests)
    set_tests_properties(UiSurfaceRemovalRaceTests PROPERTIES TIMEOUT 15)
endif()
