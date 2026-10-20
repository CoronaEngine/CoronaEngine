if(NOT DEFINED REPO_ROOT)
    message(FATAL_ERROR "REPO_ROOT is required")
endif()

# An old cache or explicit command-line OFF must not disable the renderer.
set(CORONA_BUILD_VISION OFF CACHE BOOL "Legacy setting" FORCE)
include("${REPO_ROOT}/cmake/corona_options.cmake")
if(NOT CORONA_BUILD_VISION)
    message(FATAL_ERROR "Vision must remain enabled with a legacy OFF setting")
endif()
get_property(_vision_option_type CACHE CORONA_BUILD_VISION PROPERTY TYPE)
if(NOT _vision_option_type STREQUAL "INTERNAL")
    message(FATAL_ERROR "Vision must not be exposed as an optional build feature")
endif()
