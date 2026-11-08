cmake_minimum_required(VERSION 4.0)
project(CoronaPortableHotfixFixture LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
if(MSVC)
    add_compile_options(/utf-8)
endif()
get_filename_component(CORONA_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(HORIZON_SOURCE_DIR "${CORONA_SOURCE_DIR}/Horizon" CACHE PATH "Pinned Horizon sources")
set(HORIZON_BUILD_ENGINE OFF CACHE BOOL "" FORCE)
set(HORIZON_BUILD_OCARINA OFF CACHE BOOL "" FORCE)
set(HORIZON_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(HORIZON_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(HORIZON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${HORIZON_SOURCE_DIR}" horizon)
include("${CORONA_SOURCE_DIR}/build_tools/cmake/corona_hotfix.cmake" OPTIONAL)
corona_configure_hotfix_runtime("${HORIZON_SOURCE_DIR}")

# Keep a non-ASCII watched path in the regular test, not only manual runs in
# Unicode build directories. Narrow Windows ACP conversions lose this path.
set(fixture_directory "${CMAKE_BINARY_DIR}/fixture-源码")
file(MAKE_DIRECTORY "${fixture_directory}" "${CMAKE_BINARY_DIR}/base")
# Runtime tests edit only these binary-tree copies. Do not reset them on a
# nested CMake regeneration, which would erase the patch being tested.
foreach(source fixture.cpp implementation.cpp)
    if(NOT EXISTS "${fixture_directory}/${source}")
        configure_file("${source}" "${fixture_directory}/${source}" COPYONLY)
    endif()
endforeach()
if(NOT EXISTS "${CMAKE_BINARY_DIR}/base/base.cpp")
    file(WRITE "${CMAKE_BINARY_DIR}/base/base.cpp" "int fixture_base() { return 10; }\n")
endif()
add_subdirectory(base)
add_library(fixture-normal SHARED
    "${fixture_directory}/fixture.cpp"
    "${fixture_directory}/implementation.cpp")
target_include_directories(fixture-normal PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(fixture-normal PUBLIC corona-hotfix-runtime PRIVATE fixture-base)
set_target_properties(fixture-normal PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
corona_add_hotfix_patch(fixture-normal)
corona_finalize_hotfix()

add_executable(corona-portable-hotfix-test test_runtime.cpp)
target_link_libraries(corona-portable-hotfix-test PRIVATE fixture-normal)
target_compile_definitions(corona-portable-hotfix-test PRIVATE
    FIXTURE_DIRECTORY=u8"${fixture_directory}"
    FIXTURE_BASE_SOURCE=u8"${CMAKE_BINARY_DIR}/base/base.cpp"
    FIXTURE_NORMAL_DLL=u8"$<TARGET_FILE:fixture-normal>"
    FIXTURE_BASE_DLL=u8"$<TARGET_FILE:fixture-base>"
    FIXTURE_BUILD_DIRECTORY=u8"${CMAKE_BINARY_DIR}")
enable_testing()
add_test(NAME CoronaPortableHotfixRuntime COMMAND corona-portable-hotfix-test)
set_tests_properties(CoronaPortableHotfixRuntime PROPERTIES
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:corona-portable-hotfix-test>"
    TIMEOUT 180 RUN_SERIAL TRUE)
