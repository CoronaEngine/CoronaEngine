add_library(vision-shape INTERFACE)
vision_add_plugin(quad CATEGORY shape SOURCES quad.cpp)
vision_add_plugin(cube CATEGORY shape SOURCES cube.cpp)
vision_add_plugin(sphere CATEGORY shape SOURCES sphere.cpp)
