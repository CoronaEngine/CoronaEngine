
set(CMAKE_UNITY_BUILD ${VISION_UNITY_BUILD})

add_subdirectory(filter)
add_subdirectory(sampler)
add_subdirectory(light)
add_subdirectory(lightsampler)
add_subdirectory(integrator)
add_subdirectory(material)
add_subdirectory(medium)
add_subdirectory(sensor)
add_subdirectory(shadernode)
add_subdirectory(shape)
add_subdirectory(warper)
add_subdirectory(spectrum)
add_subdirectory(tonemapper)
add_subdirectory(denoiser)
add_subdirectory(pass)
add_subdirectory(framebuffer)
add_subdirectory(upsampler)

add_library(vision-plugins INTERFACE)
target_link_libraries(vision-plugins INTERFACE
        vision-filter
        vision-shape
        vision-tonemapper
        vision-upsampler
        vision-denoiser
        vision-pass
        vision-lightsampler
        vision-sensor
        vision-light
        vision-framebuffer
        vision-shadernode
        vision-integrator
        vision-warper
        vision-spectrum
        vision-material
        vision-medium
        vision-sampler)
