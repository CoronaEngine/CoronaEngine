add_executable(corona_engine_startup_config_tests
    test_engine_startup_config.cpp
    ${PROJECT_SOURCE_DIR}/examples/engine/startup_config.cpp)
target_include_directories(corona_engine_startup_config_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/examples/engine ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(corona_engine_startup_config_tests PRIVATE nlohmann_json::nlohmann_json)
add_test(NAME EngineStartupConfigTests
    COMMAND corona_engine_startup_config_tests ${CMAKE_CURRENT_BINARY_DIR}/startup-config-fixture)
add_test(NAME EngineStartupConfigDeploymentTests
    COMMAND ${CMAKE_COMMAND}
        "-DREPO_ROOT=${PROJECT_SOURCE_DIR}"
        "-DTEST_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/startup-config-deployment"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/test_startup_config_deploy.cmake)
set_tests_properties(EngineStartupConfigTests EngineStartupConfigDeploymentTests
    PROPERTIES TIMEOUT 20)

add_executable(corona_ui_surface_removal_race_tests
    test_ui_surface_removal_race.cpp
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

if(CORONA_BUILD_VISION)
    add_executable(corona_vision_runtime_geometry_sync_tests test_vision_runtime_geometry_sync.cpp)
    target_include_directories(corona_vision_runtime_geometry_sync_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src/systems/optics
        ${CMAKE_SOURCE_DIR}/vision/src)
    target_link_libraries(corona_vision_runtime_geometry_sync_tests PRIVATE corona::engine vision-all_libs)
    add_test(NAME VisionRuntimeGeometrySyncTests COMMAND corona_vision_runtime_geometry_sync_tests)
    set_tests_properties(VisionRuntimeGeometrySyncTests PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:vision-material-principled_bsdf>")
    foreach(case fresh material topology lights stale eviction)
        add_test(NAME VisionRuntimeGeometry_${case} COMMAND corona_vision_runtime_geometry_sync_tests ${case})
        set_tests_properties(VisionRuntimeGeometry_${case} PROPERTIES
            WORKING_DIRECTORY "$<TARGET_FILE_DIR:vision-material-principled_bsdf>")
    endforeach()
    add_executable(corona_vision_embedded_mode_switch_tests
        test_vision_embedded_mode_switch.cpp
        test_vision_svgf_reprojection.cpp
        test_vision_svgf_total_albedo.cpp)
    target_include_directories(corona_vision_embedded_mode_switch_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src/systems/optics
        ${CMAKE_SOURCE_DIR}/vision/src)
    target_link_libraries(corona_vision_embedded_mode_switch_tests PRIVATE
        corona::engine vision-all_libs)
    add_test(NAME VisionEmbeddedModeSwitchTests COMMAND corona_vision_embedded_mode_switch_tests)
    add_test(NAME VisionSceneAssetReuseTests COMMAND corona_vision_embedded_mode_switch_tests --scene-asset-reuse)
    set_tests_properties(VisionSceneAssetReuseTests PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:vision-material-principled_bsdf>"
        RUN_SERIAL TRUE TIMEOUT 300)
    set_tests_properties(VisionEmbeddedModeSwitchTests PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:vision-material-principled_bsdf>")

    add_executable(corona_vision_restir_estimator_tests
        test_vision_restir_estimators.cpp
        test_vision_restir_reprojection.cpp
        test_vision_restir_continuation.cpp
        test_vision_restir_support.cpp)
    target_include_directories(corona_vision_restir_estimator_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/vision/src)
    target_link_libraries(corona_vision_restir_estimator_tests PRIVATE
        corona::engine vision-all_libs vision-integrator-ReSTIR)
    add_test(NAME VisionReSTIREstimatorTests COMMAND corona_vision_restir_estimator_tests)
    set_tests_properties(VisionReSTIREstimatorTests PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:vision-material-principled_bsdf>"
        RUN_SERIAL TRUE
        TIMEOUT 120)
    add_test(NAME VisionReSTIRVisibilityMIS
        COMMAND ${Python_EXECUTABLE} ${CMAKE_SOURCE_DIR}/vision/src/tests/test_restir_visibility_mis.py)
    set_tests_properties(VisionReSTIRVisibilityMIS PROPERTIES TIMEOUT 30)
    add_test(NAME VisionReSTIRPairwiseMIS
        COMMAND ${Python_EXECUTABLE} ${CMAKE_SOURCE_DIR}/vision/src/tests/test_restir_pairwise_mis.py)
    add_test(NAME VisionReSTIRGISupport
        COMMAND ${Python_EXECUTABLE} ${CMAKE_SOURCE_DIR}/vision/src/tests/test_restir_gi_support.py)
    set_tests_properties(VisionReSTIRPairwiseMIS VisionReSTIRGISupport PROPERTIES TIMEOUT 30)
endif()

add_executable(corona_collaborative_editor_loopback_tests
    test_collaborative_editor_loopback.cpp
    ${PROJECT_SOURCE_DIR}/src/shared_data_hub.cpp
)
target_include_directories(corona_collaborative_editor_loopback_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/include
            ${PROJECT_SOURCE_DIR}/src/systems/ui
)
target_link_libraries(corona_collaborative_editor_loopback_tests
    PRIVATE corona_ui_system corona::resource::manager
)
if(COMMAND helicon_install_runtime_deps)
    helicon_install_runtime_deps(corona_collaborative_editor_loopback_tests)
endif()
add_custom_command(TARGET corona_collaborative_editor_loopback_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<IF:$<CONFIG:Debug>,${CEF_ROOT}/Debug/libcef.dll,${CEF_ROOT}/Release/libcef.dll>"
            "$<TARGET_FILE_DIR:corona_collaborative_editor_loopback_tests>/libcef.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<IF:$<CONFIG:Debug>,${CEF_ROOT}/Debug/chrome_elf.dll,${CEF_ROOT}/Release/chrome_elf.dll>"
            "$<TARGET_FILE_DIR:corona_collaborative_editor_loopback_tests>/chrome_elf.dll"
    VERBATIM
)
add_test(NAME CollaborativeEditorLoopbackTests
         COMMAND corona_collaborative_editor_loopback_tests)
get_filename_component(_CORONA_LOOPBACK_PYTHON_DIR "${Python_EXECUTABLE}" DIRECTORY)
set(_CORONA_LOOPBACK_PATH_DIRS "${_CORONA_LOOPBACK_PYTHON_DIR}")
foreach(_tbb_runtime IN LISTS CORONA_TBB_REDIS_DLLS)
    get_filename_component(_tbb_runtime_dir "${_tbb_runtime}" DIRECTORY)
    list(APPEND _CORONA_LOOPBACK_PATH_DIRS "${_tbb_runtime_dir}")
endforeach()
list(REMOVE_DUPLICATES _CORONA_LOOPBACK_PATH_DIRS)
set(_CORONA_LOOPBACK_ENVIRONMENT)
foreach(_runtime_dir IN LISTS _CORONA_LOOPBACK_PATH_DIRS)
    list(APPEND _CORONA_LOOPBACK_ENVIRONMENT
         "PATH=path_list_prepend:${_runtime_dir}")
endforeach()
set_tests_properties(CollaborativeEditorLoopbackTests PROPERTIES
    RUN_SERIAL TRUE
    TIMEOUT 30
    ENVIRONMENT_MODIFICATION "${_CORONA_LOOPBACK_ENVIRONMENT}"
)
unset(_CORONA_LOOPBACK_PYTHON_DIR)
unset(_CORONA_LOOPBACK_PATH_DIRS)
unset(_CORONA_LOOPBACK_ENVIRONMENT)
unset(_runtime_dir)
unset(_tbb_runtime_dir)

add_executable(corona_ui_multisurface_smoke
    ui_multisurface_smoke.cpp
    ${PROJECT_SOURCE_DIR}/src/shared_data_hub.cpp
)
target_include_directories(corona_ui_multisurface_smoke
    PRIVATE ${PROJECT_SOURCE_DIR}/include
            ${PROJECT_SOURCE_DIR}/src/systems/ui
)
target_link_libraries(corona_ui_multisurface_smoke
    PRIVATE corona_ui_system corona_display_system corona_script_system corona::resource::manager
)
if(COMMAND helicon_install_runtime_deps)
    helicon_install_runtime_deps(corona_ui_multisurface_smoke)
endif()
add_test(NAME UiMultiSurfaceSmoke
         COMMAND corona_ui_multisurface_smoke)
set_tests_properties(UiMultiSurfaceSmoke PROPERTIES
    RUN_SERIAL TRUE
    SKIP_RETURN_CODE 77
    TIMEOUT 180
)
