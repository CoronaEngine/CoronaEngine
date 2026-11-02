add_library(vision-reblur-scheduler STATIC
    src/InstanceImpl.cpp
    src/Reblur.cpp
    src/Timer.cpp
    src/Wrapper.cpp)

target_include_directories(vision-reblur-scheduler
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/math")
target_compile_definitions(vision-reblur-scheduler
    PUBLIC NRD_STATIC_LIBRARY
    PRIVATE NRD_NORMAL_ENCODING=2 NRD_ROUGHNESS_ENCODING=1)
set_target_properties(vision-reblur-scheduler PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)

if(MSVC)
    # Match the supplied NRD 4.14.3 CPU build's compiler and floating-point mode.
    # Do not select AVX/FMA or fast math: constants are compared byte-for-byte.
    target_compile_options(vision-reblur-scheduler PRIVATE /GR /W4 /wd4324 /fp:precise)
    target_compile_definitions(vision-reblur-scheduler PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
        _UNICODE UNICODE _ENFORCE_MATCHING_ALLOCATORS=0)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    target_compile_options(vision-reblur-scheduler PRIVATE -msse4.1)
else()
    message(FATAL_ERROR "The bundled REBLUR CPU scheduler currently requires an x86-64 compiler; no external SIMD SDK is downloaded.")
endif()
