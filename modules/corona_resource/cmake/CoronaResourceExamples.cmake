# =============================================================================
# CoronaResource Examples Helper Functions
# =============================================================================

include_guard(GLOBAL)

# Function: corona_add_example
# Add a Corona example executable with common configuration
#
# Parameters:
#   NAME - Example target name
#   SOURCES - List of source files
#   LIBRARIES - List of libraries to link
#   USD_MODULES - List of USD modules to link (optional)
#   NEEDS_TBB - Whether to copy TBB runtime artifacts (default: OFF)
#   NEEDS_ASSIMP - Whether to copy Assimp runtime artifacts (default: OFF)
#   NEEDS_USD_COPY - Whether to copy USD files (default: OFF)
#
# Example usage:
#   corona_add_example(
#       NAME SceneExample
#       SOURCES scene_example.cpp
#       LIBRARIES corona::resource::manager
#       NEEDS_TBB ON
#       NEEDS_ASSIMP ON
#   )
function(corona_add_example)
    # Parse arguments
    set(options NEEDS_TBB NEEDS_ASSIMP)
    set(oneValueArgs NAME)
    set(multiValueArgs SOURCES LIBRARIES)
    cmake_parse_arguments(EXAMPLE "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    # Validate required arguments
    if(NOT EXAMPLE_NAME)
        message(FATAL_ERROR "corona_add_example: NAME is required")
    endif()

    if(NOT EXAMPLE_SOURCES)
        message(FATAL_ERROR "corona_add_example: SOURCES is required")
    endif()

    # Create executable
    add_executable(${EXAMPLE_NAME} ${EXAMPLE_SOURCES})

    # Link libraries
    if(EXAMPLE_LIBRARIES)
        target_link_libraries(${EXAMPLE_NAME} PRIVATE ${EXAMPLE_LIBRARIES})
    endif()

    # Set C++ standard
    target_compile_features(${EXAMPLE_NAME} PRIVATE cxx_std_20)

    # Provide project source dir to the example for locating test assets
    target_compile_definitions(${EXAMPLE_NAME}
        PRIVATE
        CORONARESOURCE_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"
    )

    # Copy runtime files from all linked libraries
    # This ensures that DLLs from all dependencies (including transitive USD dependencies) are copied
    if(EXAMPLE_LIBRARIES)
        foreach(_lib ${EXAMPLE_LIBRARIES})
            if(TARGET ${_lib})
                corona_copy_runtime_files(${_lib} ${EXAMPLE_NAME})
            endif()
        endforeach()
    endif()

    # Copy TBB runtime artifacts if needed
    if(EXAMPLE_NEEDS_TBB)
        corona_copy_tbb_runtime_artifacts(${EXAMPLE_NAME})
    endif()

    # Copy Assimp runtime artifacts if needed (if not already copied via LIBRARIES)
    if(EXAMPLE_NEEDS_ASSIMP AND NOT "assimp::assimp" IN_LIST EXAMPLE_LIBRARIES)
        corona_copy_runtime_files(assimp::assimp ${EXAMPLE_NAME})
    endif()

    message(STATUS "[corona_add_example] Added example: ${EXAMPLE_NAME}")
endfunction()

