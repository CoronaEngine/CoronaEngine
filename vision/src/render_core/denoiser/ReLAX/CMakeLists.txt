add_library(vision-relax-filter STATIC filter.cpp modulator.cpp prepass.cpp temporal_accumulator.cpp
    history_fix.cpp history_clamping.cpp)
target_include_directories(vision-relax-filter PUBLIC ${CMAKE_SOURCE_DIR}/vision/src)
target_link_libraries(vision-relax-filter PUBLIC ocarina-native ocarina-include)
