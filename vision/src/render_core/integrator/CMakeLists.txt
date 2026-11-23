add_library(vision-integrator INTERFACE)

add_subdirectory(restir)
add_subdirectory(adaptive)

vision_add_plugin(pt CATEGORY integrator DEPENDENCE "vision-integrator-adaptive" SOURCES pt.cpp)
vision_add_plugin(rt CATEGORY integrator DEPENDENCE "vision-integrator-ReSTIR" SOURCES rt.cpp)
