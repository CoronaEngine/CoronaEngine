add_subdirectory(scheduler)
add_library(vision-reblur-kernels STATIC filter.cpp modulator.cpp prepass.cpp
    temporal_accumulator.cpp history_fix.cpp spatial_filter.cpp temporal_stabilizer.cpp
    edsl_spatial.cpp edsl_history_fix.cpp edsl_temporal.cpp edsl_conversion.cpp edsl_nrd.cpp)
target_link_libraries(vision-reblur-kernels PUBLIC vision-base vision-math vision-reblur-scheduler)
target_compile_definitions(vision-reblur-kernels PUBLIC VISION_REBLUR_EDSL=1)
vision_add_plugin(ReBLUR CATEGORY denoiser SOURCES reblur.cpp DEPENDENCE vision-reblur-kernels)
# Retire cached SDK settings: this target is entirely repository-local.
unset(VISION_REBLUR_NATIVE_NRD CACHE)
unset(VISION_NATIVE_NRD_DEPENDENCIES CACHE)
unset(VISION_NATIVE_NRD_DXC CACHE)
