cmake_minimum_required(VERSION 4.0)

foreach(required REPO_ROOT TEST_DIRECTORY GENERATOR CONFIGURATION CTEST_COMMAND)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "Portable hotfix test requires ${required}")
    endif()
endforeach()

# Use the same compiler environment as Engine, including background patch builds.
if(EXISTS "${BUILD_ENVIRONMENT}")
    include("${BUILD_ENVIRONMENT}")
elseif(WIN32)
    message(FATAL_ERROR "Missing Engine build environment: ${BUILD_ENVIRONMENT}")
endif()

set(configure_command "${CMAKE_COMMAND}"
    -S "${REPO_ROOT}/tests/integration/portable_hotfix"
    -B "${TEST_DIRECTORY}" -G "${GENERATOR}"
    "-DCMAKE_BUILD_TYPE=${CONFIGURATION}"
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON)
foreach(option TOOLCHAIN_FILE MAKE_PROGRAM)
    if(DEFINED ${option} AND NOT "${${option}}" STREQUAL "")
        list(APPEND configure_command "-DCMAKE_${option}=${${option}}")
    endif()
endforeach()
if(GENERATOR_PLATFORM)
    list(APPEND configure_command -A "${GENERATOR_PLATFORM}")
endif()
if(GENERATOR_TOOLSET)
    list(APPEND configure_command -T "${GENERATOR_TOOLSET}")
endif()

# The fixture has its own reduced Horizon options. Configure it independently;
# all dependencies must already be available through Engine's selected toolchain.
execute_process(COMMAND ${configure_command} RESULT_VARIABLE result)
if(NOT result STREQUAL "0")
    message(FATAL_ERROR "Portable hotfix fixture configure failed: ${result}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${TEST_DIRECTORY}"
    --config "${CONFIGURATION}" RESULT_VARIABLE result)
if(NOT result STREQUAL "0")
    message(FATAL_ERROR "Portable hotfix fixture build failed: ${result}")
endif()
execute_process(COMMAND "${CTEST_COMMAND}" --test-dir "${TEST_DIRECTORY}"
    -C "${CONFIGURATION}" --output-on-failure --no-tests=error
    RESULT_VARIABLE result)
if(NOT result STREQUAL "0")
    message(FATAL_ERROR "Portable hotfix fixture tests failed: ${result}")
endif()
