add_library(vision-reblur-kernels STATIC filter.cpp modulator.cpp prepass.cpp
    temporal_accumulator.cpp history_fix.cpp spatial_filter.cpp temporal_stabilizer.cpp)
target_link_libraries(vision-reblur-kernels PUBLIC vision-base vision-math)
vision_add_plugin(ReBLUR CATEGORY denoiser SOURCES reblur.cpp DEPENDENCE vision-reblur-kernels)
include(native_nrd.cmake)
