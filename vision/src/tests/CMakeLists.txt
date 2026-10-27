
function(vision_add_test name)
    cmake_parse_arguments(TEST "" "" "SOURCES" ${ARGN})
    add_executable(${name}  ${TEST_SOURCES})
    target_link_libraries(${name} vision-all_libs)
    set_target_properties(${name} PROPERTIES
            PUBLIC_HEADER "${HEADER_FILES}"
            LIBRARY_OUTPUT_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
            WINDOWS_EXPORT_ALL_SYMBOLS ON)
endfunction()

vision_add_test(test-playground SOURCES test_playground.cpp)
vision_add_test(test-warper2d SOURCES test_warper2d.cpp)

vision_add_test(test-render_graph SOURCES test_render_graph.cpp)
vision_add_test(test-thin-lens SOURCES test_thin_lens.cpp)

vision_add_test(test-material-energy SOURCES test_material_energy.cpp)
vision_add_test(test-material-reset SOURCES test_material_reset.cpp)

vision_add_test(test-shader-graph-lifecycle SOURCES test_shader_graph_lifecycle.cpp)
add_dependencies(test-shader-graph-lifecycle
    vision-material-diffuse
    vision-light-point
    vision-shadernode-number)
vision_add_test(test-param_schema SOURCES test_param_schema.cpp)

function(vision_add_simple_test name)
    cmake_parse_arguments(TEST "" "" "SOURCES" ${ARGN})

    add_executable(${name}  ${TEST_SOURCES})
    target_link_libraries(${name} vision-hotfix-all vision-window)
    set_target_properties(${name} PROPERTIES
            PUBLIC_HEADER "${HEADER_FILES}"
            LIBRARY_OUTPUT_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
            WINDOWS_EXPORT_ALL_SYMBOLS ON)
endfunction()

vision_add_simple_test(vision-test_hotfix SOURCES test_hotfix.cpp)

#if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_sdk.cpp)
#    add_executable(vision-test_sdk test_sdk.cpp)
#    add_dependencies(vision-test_sdk vision-renderer)
#else()
#    message(WARNING "Skipping vision-test_sdk: missing source file test_sdk.cpp")
#endif()
