option(VISION_REBLUR_NATIVE_NRD "Build the native Vulkan NRD backend" OFF)
set(VISION_NATIVE_NRD_DEPENDENCIES "" CACHE FILEPATH "NativeNrdDependencies.cmake with matching NRD and NVRHI targets")
unset(VISION_NATIVE_NRD_LIBRARY_DIR CACHE)

if(VISION_REBLUR_NATIVE_NRD)
    if(NOT EXISTS "${VISION_NATIVE_NRD_DEPENDENCIES}")
        message(FATAL_ERROR "VISION_NATIVE_NRD_DEPENDENCIES must point to matching NRD 4.14.3 SPIR-V and NVRHI dependencies")
    endif()
    include("${VISION_NATIVE_NRD_DEPENDENCIES}")
    get_filename_component(_native_nrd_dependency_root "${VISION_NATIVE_NRD_DEPENDENCIES}/../.." ABSOLUTE)
    find_program(VISION_NATIVE_NRD_DXC NAMES dxc HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)
    add_library(vision-reblur-native STATIC native_nrd.cpp)
    target_include_directories(vision-reblur-native BEFORE PRIVATE
        "${_native_nrd_dependency_root}/NVRHI/thirdparty/Vulkan-Headers/include")
    target_link_libraries(vision-reblur-native PUBLIC vision-base vision-math PRIVATE NativeNrd::Nrd NativeNrd::Vulkan)
    target_compile_definitions(vision-reblur-native PRIVATE VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1)
    target_compile_definitions(vision-reblur-kernels PUBLIC VISION_REBLUR_NATIVE_NRD=1)
    target_link_libraries(vision-reblur-kernels PUBLIC vision-reblur-native)
    target_sources(vision-reblur-kernels PRIVATE edsl_spatial.cpp edsl_history_fix.cpp edsl_temporal.cpp edsl_conversion.cpp edsl_nrd.cpp)
    target_link_libraries(vision-reblur-kernels PRIVATE NativeNrd::Nrd)

    set(_native_reblur_shader_outputs)
    foreach(_shader IN ITEMS NativeReblurPack NativeReblurUnpack)
        set(_output "${CMAKE_CURRENT_BINARY_DIR}/native-shaders/${_shader}.spv")
        add_custom_command(OUTPUT "${_output}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/native-shaders"
            COMMAND "${VISION_NATIVE_NRD_DXC}" -T cs_6_6 -E main -spirv -fspv-target-env=vulkan1.2
                -fvk-s-shift 100 0 -fvk-t-shift 200 0 -fvk-b-shift 300 0 -fvk-u-shift 400 0
                -D NRD_NORMAL_ENCODING=2 -D NRD_ROUGHNESS_ENCODING=1
                -I "${_native_nrd_dependency_root}/NRD/Shaders/Include"
                "${CMAKE_CURRENT_SOURCE_DIR}/${_shader}.hlsl" -Fo "${_output}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${_shader}.hlsl"
                "${_native_nrd_dependency_root}/NRD/Shaders/Include/NRD.hlsli"
            VERBATIM)
        list(APPEND _native_reblur_shader_outputs "${_output}")
    endforeach()
    add_custom_target(vision-reblur-native-shaders DEPENDS ${_native_reblur_shader_outputs})
    add_dependencies(vision-denoiser-ReBLUR vision-reblur-native-shaders)
    add_custom_command(TARGET vision-denoiser-ReBLUR POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:vision-denoiser-ReBLUR>/reblur-native"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${_native_reblur_shader_outputs}
            "$<TARGET_FILE_DIR:vision-denoiser-ReBLUR>/reblur-native"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:NativeNrd::Nrd>"
            "$<TARGET_FILE_DIR:vision-denoiser-ReBLUR>"
        VERBATIM)
endif()
