
file(GLOB_RECURSE HEADER_FILES *.h*)
file(GLOB_RECURSE SOURCE_FILES *.c*)

add_library(vision-base SHARED ${SOURCE_FILES})
target_link_libraries(vision-base PUBLIC
        ocarina
        vision-window
        vision-UI
        vision-ext
        ocarina-include
        corona-hotfix-runtime)

target_compile_definitions(vision-base PRIVATE
        VISION_BASE_EXPORT_DLL=1
        VISION_INTERACTIVE_AUX_WORK=$<BOOL:${VISION_INTERACTIVE_AUX_WORK}>)
set_target_properties(vision-base PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
        WINDOWS_EXPORT_ALL_SYMBOLS ON)

# The registered Visualizer and its sensor implementation group were the
# original base watcher scope. Keep process-global managers in the stable base
# DLL; the patch imports those managers through its immutable link snapshot.
file(GLOB SENSOR_HOTFIX_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/sensor/*.cpp")
corona_add_hotfix_patch(vision-base SOURCES ${SENSOR_HOTFIX_SOURCES}
    LINK_LIBRARIES vision-base)

