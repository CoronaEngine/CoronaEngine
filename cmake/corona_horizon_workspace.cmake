include_guard(GLOBAL)

function(corona_add_horizon_workspace)
    set(_corona_horizon_source "${CMAKE_SOURCE_DIR}/.workspace/Horizon")
    if(NOT EXISTS "${_corona_horizon_source}/CMakeLists.txt")
        message(FATAL_ERROR
            "Horizon workspace was not prepared at ${_corona_horizon_source}. "
            "Run 'conda run -n coronaengine-dev python tools/dev.py configure'.")
    endif()

    set(HORIZON_BUILD_TOOLS ON CACHE BOOL "" FORCE)
    set(HORIZON_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(HORIZON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(HORIZON_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
    set(HORIZON_BUILD_OCARINA_TESTS OFF CACHE BOOL "" FORCE)
    set(HORIZON_BUILD_OCARINA ${CORONA_BUILD_VISION} CACHE BOOL "" FORCE)
    set(HORIZON_BUILD_VISION_HOTFIX ${CORONA_BUILD_VISION} CACHE BOOL "" FORCE)

    add_subdirectory(
        "${_corona_horizon_source}"
        "${CMAKE_BINARY_DIR}/workspace/horizon")
    if(MSVC AND TARGET ocarina-backend-cuda)
        target_compile_options(ocarina-backend-cuda PRIVATE /FIio.h)
    endif()
endfunction()
