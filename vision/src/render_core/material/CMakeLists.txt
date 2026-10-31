add_library(vision-material INTERFACE)
vision_add_plugin(black_body CATEGORY material SOURCES black_body.cpp)
vision_add_plugin(principled_bsdf CATEGORY material SOURCES principled_bsdf.cpp)
